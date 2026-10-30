"""Async RPC client for protocol 2.

One connection, many in-flight requests: the reader task matches responses by
``id``, so callers may await concurrently.  The server executes UI requests on
the GUI thread in FIFO order; response order is not guaranteed.
"""
from __future__ import annotations

import asyncio
import logging
from collections.abc import Mapping
from contextlib import suppress
from dataclasses import dataclass
from typing import Any

from .errors import ErrorCode
from .protocol import (
    MAX_FRAME_BYTES,
    PeerClosed,
    ProtocolError,
    encode_frame,
    make_request,
    read_frame,
)

logger = logging.getLogger(__name__)

PROTOCOL_MAJOR = 2
DEFAULT_TIMEOUT = 30.0
DEFAULT_HELLO_TIMEOUT = 5.0


class RpcError(Exception):
    """A structured error returned by the server."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        cause: str | None = None,
        retryable: bool = False,
        hint: str | None = None,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.cause = cause
        self.retryable = retryable
        self.hint = hint
        self.details = dict(details or {})

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> "RpcError":
        code = payload.get("code")
        message = payload.get("message")
        if not isinstance(code, str) or not code:
            code = ErrorCode.PROTOCOL.value
        if not isinstance(message, str):
            message = "invalid error payload"
        cause = payload.get("cause")
        hint = payload.get("hint")
        details = payload.get("details")
        return cls(
            code,
            message,
            cause=cause if isinstance(cause, str) else None,
            retryable=bool(payload.get("retryable", False)),
            hint=hint if isinstance(hint, str) else None,
            details=details if isinstance(details, Mapping) else None,
        )


class ClientClosed(Exception):
    """The connection is closed or was lost."""


class ClientTimeout(Exception):
    """No response arrived before the request deadline."""

    def __init__(self, request_id: int, method: str, timeout: float) -> None:
        super().__init__(f"{method} (id={request_id}) timed out after {timeout:.3f}s")
        self.request_id = request_id
        self.method = method
        self.timeout = timeout


@dataclass(frozen=True, slots=True)
class FrameResponse:
    """A response plus its optional binary segment (e.g. a PNG)."""

    result: dict[str, Any]
    binary: bytes = b""


@dataclass(frozen=True, slots=True)
class ServerHello:
    protocol: int
    library_version: str
    capabilities: tuple[str, ...]
    session_tag: str
    max_inflight: int
    max_frame: int


class RpcClient:
    """A single connection.

    Use :meth:`connect` so the ``hello`` handshake is validated before the
    instance is returned.
    """

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        *,
        max_inflight: int = 8,
        default_timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        if max_inflight < 1:
            raise ValueError("max_inflight must be >= 1")
        self._reader = reader
        self._writer: asyncio.StreamWriter | None = writer
        self._default_timeout = default_timeout
        self._client_max_inflight = max_inflight
        self._max_inflight = max_inflight
        self._max_frame = MAX_FRAME_BYTES
        self._semaphore = asyncio.Semaphore(max_inflight)
        self._send_lock = asyncio.Lock()
        self._close_lock = asyncio.Lock()
        self._limits_applied = asyncio.Event()
        self._pending: dict[
            int, asyncio.Future[tuple[dict[str, Any], bytes]]
        ] = {}
        self._next_id = 1
        self._closed = False
        self._reader_task = asyncio.create_task(
            self._read_loop(), name="qt-commander-reader"
        )
        self.hello: ServerHello | None = None

    @classmethod
    async def connect(
        cls,
        host: str,
        port: int,
        token: str,
        *,
        client_name: str = "qt-commander",
        hello_timeout: float = DEFAULT_HELLO_TIMEOUT,
        default_timeout: float = DEFAULT_TIMEOUT,
        max_inflight: int = 8,
    ) -> "RpcClient":
        if not token:
            raise ValueError("token must not be empty")
        reader, writer = await asyncio.open_connection(host, port)
        client = cls(
            reader,
            writer,
            max_inflight=max_inflight,
            default_timeout=default_timeout,
        )
        try:
            result = await client.call(
                "hello",
                {"protocol": PROTOCOL_MAJOR, "token": token, "client": client_name},
                timeout=hello_timeout,
            )
            client.hello = _parse_hello(result)
            client._apply_hello_limits(client.hello)
        except BaseException:
            await client.close()
            raise
        return client

    async def __aenter__(self) -> "RpcClient":
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        await self.close()

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def connected(self) -> bool:
        return not self._closed

    @property
    def max_inflight(self) -> int:
        return self._max_inflight

    @property
    def max_frame(self) -> int:
        return self._max_frame

    def _apply_hello_limits(self, hello: ServerHello) -> None:
        # Never let a peer raise the protocol's hard frame cap.
        self._max_frame = min(hello.max_frame, MAX_FRAME_BYTES)
        # The server limit is authoritative; a stricter local limit still wins.
        effective = min(self._client_max_inflight, hello.max_inflight)
        self._max_inflight = effective
        self._semaphore = asyncio.Semaphore(effective)
        self._limits_applied.set()

    async def call(
        self,
        method: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        op_id: str | None = None,
    ) -> dict[str, Any]:
        """Send one request and return its JSON result (binary discarded)."""
        response = await self.call_frame(
            method, params, timeout=timeout, op_id=op_id
        )
        return response.result

    async def call_frame(
        self,
        method: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        op_id: str | None = None,
    ) -> FrameResponse:
        """Send one request and return both result and binary payload.

        Raises :class:`RpcError` for server errors, :class:`ClientTimeout` when
        the deadline expires, and :class:`ClientClosed` when the connection is
        gone.
        """
        if self._closed:
            raise ClientClosed("client is closed")
        wait = self._default_timeout if timeout is None else timeout
        if wait <= 0:
            raise ValueError("timeout must be > 0")

        async with self._semaphore:
            if self._closed:
                raise ClientClosed("client is closed")
            request_id = self._next_id
            self._next_id += 1
            future: asyncio.Future[tuple[dict[str, Any], bytes]] = (
                asyncio.get_running_loop().create_future()
            )
            self._pending[request_id] = future
            try:
                request = make_request(
                    request_id,
                    method,
                    params,
                    timeout_ms=int(wait * 1000),
                    op_id=op_id,
                )
                await self._send(request)
                result, binary = await asyncio.wait_for(future, timeout=wait)
                return FrameResponse(result=result, binary=binary)
            except asyncio.TimeoutError:
                with suppress(ClientClosed, ConnectionError, OSError, ProtocolError):
                    await self._send_cancel(request_id)
                raise ClientTimeout(request_id, method, wait) from None
            except asyncio.CancelledError:
                with suppress(
                    asyncio.CancelledError,
                    ClientClosed,
                    ConnectionError,
                    OSError,
                    ProtocolError,
                ):
                    await self._send_cancel(request_id)
                raise
            except ProtocolError as exc:
                raise RpcError(exc.code, str(exc)) from exc
            except (ConnectionError, OSError) as exc:
                # Remove our own future first so _fail_all does not leave an
                # un-retrieved exception behind.
                self._pending.pop(request_id, None)
                if not future.done():
                    future.cancel()
                raise self._abort(f"send failed: {exc}") from exc
            finally:
                self._pending.pop(request_id, None)

    async def ping(self, *, timeout: float = 5.0) -> dict[str, Any]:
        return await self.call("ping", {}, timeout=timeout)

    async def cancel(self, target_id: int, *, timeout: float = 5.0) -> dict[str, Any]:
        return await self.call(
            "cancel", {"target_id": target_id}, timeout=timeout
        )

    async def shutdown(self, *, timeout: float = 5.0) -> None:
        if self._closed:
            return
        with suppress(RpcError, ClientClosed, ClientTimeout):
            await self.call("shutdown", {}, timeout=timeout)
        await self.close()

    async def close(self) -> None:
        """Close the connection and fail every pending request (idempotent)."""
        async with self._close_lock:
            if self._writer is None and self._reader_task.done():
                return
            self._closed = True
            self._fail_all(ClientClosed("client closed"))
            task = self._reader_task
            if not task.done():
                task.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await task
            writer = self._writer
            self._writer = None
            if writer is not None:
                writer.close()
                with suppress(ConnectionError, OSError):
                    await writer.wait_closed()

    def _fail(self, reason: str) -> None:
        """Mark closed, fail pending requests, and close the transport."""
        self._closed = True
        self._fail_all(ClientClosed(reason))
        writer = self._writer
        self._writer = None
        if writer is not None:
            writer.close()

    def _abort(self, reason: str) -> ClientClosed:
        self._fail(reason)
        task = self._reader_task
        if task is not asyncio.current_task() and not task.done():
            task.cancel()
        return ClientClosed(reason)

    async def _send(self, message: Mapping[str, Any]) -> None:
        async with self._send_lock:
            writer = self._writer
            if writer is None:
                raise ClientClosed("connection is closed")
            writer.write(encode_frame(message))
            await writer.drain()

    async def _send_cancel(self, target_id: int) -> None:
        """Best-effort cancel; its response is intentionally not awaited."""
        if self._closed:
            return
        cancel_id = self._next_id
        self._next_id += 1
        try:
            await self._send(
                make_request(
                    cancel_id,
                    "cancel",
                    {"target_id": target_id},
                    timeout_ms=1000,
                )
            )
        except (ClientClosed, ConnectionError, OSError):
            return

    async def _read_loop(self) -> None:
        first_frame = True
        try:
            while not self._closed:
                if first_frame:
                    # The hello frame must be readable with the protocol cap;
                    # its own max_frame only governs subsequent frames.
                    frame = await read_frame(
                        self._reader, max_frame=MAX_FRAME_BYTES
                    )
                    first_frame = False
                else:
                    await self._limits_applied.wait()
                    frame = await read_frame(
                        self._reader, max_frame=self._max_frame
                    )
                self._dispatch(frame.message, frame.binary)
        except asyncio.CancelledError:
            raise
        except PeerClosed as exc:
            logger.debug("peer closed: %s", exc)
            self._fail("connection closed by peer")
        except ProtocolError as exc:
            logger.warning("protocol error: %s", exc)
            self._fail(f"protocol error: {exc}")
        except (ConnectionError, OSError, asyncio.IncompleteReadError) as exc:
            self._fail(f"connection lost: {exc}")
        except Exception as exc:  # pragma: no cover - defensive
            logger.exception("reader task failed")
            self._fail(f"reader failed: {exc}")
        finally:
            if self._pending:
                self._fail_all(ClientClosed("reader stopped"))

    def _dispatch(self, message: Mapping[str, Any], binary: bytes = b"") -> None:
        request_id = message.get("id")
        if type(request_id) is not int:
            logger.debug("ignoring response without a valid id: %r", message)
            return
        future = self._pending.get(request_id)
        if future is None or future.done():
            # Late response after a timeout/cancel: intentionally dropped.
            return
        error = message.get("error")
        if isinstance(error, Mapping):
            future.set_exception(RpcError.from_payload(error))
            return
        result = message.get("result")
        if not isinstance(result, Mapping):
            future.set_exception(
                RpcError(ErrorCode.PROTOCOL.value, "response result must be an object")
            )
            return
        future.set_result((dict(result), binary))

    def _fail_all(self, exc: ClientClosed) -> None:
        pending = list(self._pending.values())
        self._pending.clear()
        for future in pending:
            if not future.done():
                future.set_exception(ClientClosed(str(exc)))


def _parse_hello(result: Mapping[str, Any]) -> ServerHello:
    protocol = result.get("protocol")
    if type(protocol) is not int or protocol != PROTOCOL_MAJOR:
        raise RpcError(
            ErrorCode.PROTOCOL.value,
            f"unsupported protocol: {protocol!r} (expected {PROTOCOL_MAJOR})",
        )
    capabilities = result.get("capabilities", [])
    if not isinstance(capabilities, list) or not all(
        isinstance(cap, str) for cap in capabilities
    ):
        raise RpcError(
            ErrorCode.PROTOCOL.value, "capabilities must be a list of strings"
        )
    limits = result.get("limits", {})
    if not isinstance(limits, Mapping):
        raise RpcError(ErrorCode.PROTOCOL.value, "limits must be an object")
    max_inflight = limits.get("max_inflight", 8)
    max_frame = limits.get("max_frame", MAX_FRAME_BYTES)
    if type(max_inflight) is not int or max_inflight < 1:
        raise RpcError(
            ErrorCode.PROTOCOL.value,
            "limits.max_inflight must be a positive int",
        )
    if type(max_frame) is not int or max_frame < 1024:
        raise RpcError(
            ErrorCode.PROTOCOL.value,
            "limits.max_frame must be a positive int",
        )
    session_tag = result.get("session_tag", "")
    if not isinstance(session_tag, str) or not session_tag:
        raise RpcError(
            ErrorCode.PROTOCOL.value, "session_tag must be a non-empty string"
        )
    library_version = result.get("library_version", "")
    if not isinstance(library_version, str):
        raise RpcError(
            ErrorCode.PROTOCOL.value, "library_version must be a string"
        )
    return ServerHello(
        protocol=protocol,
        library_version=library_version,
        capabilities=tuple(capabilities),
        session_tag=session_tag,
        max_inflight=max_inflight,
        max_frame=max_frame,
    )

