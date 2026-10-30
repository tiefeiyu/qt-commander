"""Unit tests for the async RPC client against a fake server."""
from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager, suppress
from typing import AsyncIterator, Awaitable, Callable

import pytest

from qt_commander.client import (
    ClientClosed,
    ClientTimeout,
    RpcClient,
    RpcError,
)
from qt_commander.protocol import (
    MAX_FRAME_BYTES,
    encode_frame,
    make_error,
    make_result,
    read_frame,
)

HELLO = {
    "protocol": 2,
    "library_version": "2.0.0",
    "capabilities": ["qml", "expect_js"],
    "session_tag": "7f3a",
    "limits": {"max_inflight": 8, "max_frame": 67108864},
}

Handler = Callable[[asyncio.StreamReader, asyncio.StreamWriter], Awaitable[None]]


async def _send(writer: asyncio.StreamWriter, message: dict) -> None:
    writer.write(encode_frame(message))
    await writer.drain()


@asynccontextmanager
async def _server(handler: Handler) -> AsyncIterator[int]:
    async def wrapped(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        try:
            await handler(reader, writer)
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
            with suppress(ConnectionError, OSError):
                await writer.wait_closed()

    server = await asyncio.start_server(wrapped, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        yield port
    finally:
        server.close()
        await server.wait_closed()


@pytest.mark.asyncio
async def test_hello_and_call():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        assert hello.message["method"] == "hello"
        assert hello.message["params"]["protocol"] == 2
        await _send(writer, make_result(hello.message["id"], HELLO))
        request = await read_frame(reader)
        assert request.message["method"] == "ping"
        await _send(writer, make_result(request.message["id"], {"ok": True, "pong": 1}))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            assert client.hello is not None
            assert client.hello.protocol == 2
            assert client.hello.capabilities == ("qml", "expect_js")
            assert await client.ping() == {"ok": True, "pong": 1}
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_error_response_is_mapped():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        request = await read_frame(reader)
        await _send(
            writer,
            make_error(
                request.message["id"],
                "E_NOT_FOUND",
                "no match",
                cause="no_match",
                retryable=True,
                hint="try text_contains",
                details={"count": 0},
            ),
        )

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(RpcError) as info:
                await client.call("query", {"limit": 1})
            error = info.value
            assert error.code == "E_NOT_FOUND"
            assert error.cause == "no_match"
            assert error.retryable is True
            assert error.hint == "try text_contains"
            assert error.details == {"count": 0}
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_bad_protocol_is_rejected():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        bad = dict(HELLO, protocol=1)
        await _send(writer, make_result(hello.message["id"], bad))

    async with _server(handle) as port:
        with pytest.raises(RpcError) as info:
            await RpcClient.connect("127.0.0.1", port, "token")
        assert info.value.code == "E_PROTOCOL"


@pytest.mark.asyncio
async def test_timeout_raises_and_sends_cancel():
    release = asyncio.Event()
    seen: list[int] = []

    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        request = await read_frame(reader)
        seen.append(request.message["id"])
        await asyncio.wait_for(read_frame(reader), timeout=2.0)
        release.set()

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(ClientTimeout):
                await client.call("query", timeout=0.05)
            await asyncio.wait_for(release.wait(), timeout=2.0)
            assert seen
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_hello_limits_are_applied():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        limits = dict(HELLO, limits={"max_inflight": 2, "max_frame": 4096})
        await _send(writer, make_result(hello.message["id"], limits))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            assert client.max_inflight == 2
            assert client.max_frame == 4096
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_hello_max_frame_applies_to_next_frame():
    huge = {"id": 999, "result": {"payload": "x" * 5000}}

    async def handle(reader, writer):
        hello = await read_frame(reader)
        limits = dict(HELLO, limits={"max_inflight": 8, "max_frame": 4096})
        await _send(writer, make_result(hello.message["id"], limits))
        await read_frame(reader)
        await _send(writer, huge)

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(ClientClosed, match="frame too large"):
                await client.ping(timeout=2)
            assert client.closed is True
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_hello_max_frame_is_clamped_to_protocol_cap():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        limits = dict(HELLO, limits={"max_inflight": 8, "max_frame": 10**9})
        await _send(writer, make_result(hello.message["id"], limits))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            assert client.max_frame == MAX_FRAME_BYTES
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_hello_max_inflight_caps_concurrency():
    active = 0
    peak = 0
    send_lock = asyncio.Lock()

    async def handle(reader, writer):
        nonlocal active, peak
        hello = await read_frame(reader)
        limits = dict(
            HELLO, limits={"max_inflight": 2, "max_frame": MAX_FRAME_BYTES}
        )
        await _send(writer, make_result(hello.message["id"], limits))
        tasks: set[asyncio.Task] = set()

        async def respond(request):
            nonlocal active, peak
            active += 1
            peak = max(peak, active)
            await asyncio.sleep(0.05)
            async with send_lock:
                await _send(
                    writer, make_result(request.message["id"], {"ok": True})
                )
            active -= 1

        try:
            while True:
                request = await read_frame(reader)
                tasks.add(asyncio.create_task(respond(request)))
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            await asyncio.gather(*(client.call("ping") for _ in range(6)))
            await asyncio.sleep(0.1)
            assert peak == 2
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_local_max_inflight_wins_over_hello():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        limits = dict(HELLO, limits={"max_inflight": 8, "max_frame": MAX_FRAME_BYTES})
        await _send(writer, make_result(hello.message["id"], limits))

    async with _server(handle) as port:
        client = await RpcClient.connect(
            "127.0.0.1", port, "token", max_inflight=1
        )
        try:
            assert client.max_inflight == 1
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_send_failure_fails_all_pending(monkeypatch):
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        with suppress(ConnectionError, asyncio.IncompleteReadError):
            await read_frame(reader)

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        original_send = client._send
        calls = 0

        async def flaky_send(message):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise ConnectionResetError("boom")
            await original_send(message)

        monkeypatch.setattr(client, "_send", flaky_send)
        try:
            results = await asyncio.wait_for(
                asyncio.gather(
                    client.call("one", timeout=5),
                    client.call("two", timeout=5),
                    return_exceptions=True,
                ),
                timeout=2,
            )
            assert all(isinstance(item, ClientClosed) for item in results)
            assert client.closed is True
            with pytest.raises(ClientClosed):
                await client.call("after", timeout=0.1)
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_caller_cancellation_sends_cancel():
    seen: list[dict] = []
    cancel_seen = asyncio.Event()

    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        first = await read_frame(reader)
        seen.append(first.message)
        cancel = await asyncio.wait_for(read_frame(reader), timeout=2.0)
        seen.append(cancel.message)
        cancel_seen.set()
        ping = await read_frame(reader)
        await _send(writer, make_result(ping.message["id"], {"pong": 1}))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(
                    client.call("query", timeout=10), timeout=0.1
                )
            await asyncio.wait_for(cancel_seen.wait(), timeout=2.0)
            assert seen[0]["method"] == "query"
            assert seen[1]["method"] == "cancel"
            assert seen[1]["params"]["target_id"] == seen[0]["id"]
            assert await client.ping() == {"pong": 1}
            assert client._pending == {}
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_clean_peer_close_is_not_a_protocol_error(caplog):
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(ClientClosed, match="peer"):
                await client.call("ping", timeout=2)
            warnings = [
                record for record in caplog.records
                if record.levelno >= logging.WARNING
            ]
            assert warnings == []
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_shutdown_has_no_warnings(caplog):
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        request = await read_frame(reader)
        assert request.message["method"] == "shutdown"
        await _send(writer, make_result(request.message["id"], {}))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        with caplog.at_level(logging.DEBUG):
            await client.shutdown()
        warnings = [
            record for record in caplog.records
            if record.levelno >= logging.WARNING
        ]
        assert warnings == []


@pytest.mark.asyncio
async def test_non_serializable_params_raise_rpc_error():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(RpcError) as info:
                await client.call("query", {"value": float("nan")})
            assert info.value.code == "E_PROTOCOL"
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_call_frame_returns_binary_payload():
    png = b"\x89PNG\r\n\x1a\n"

    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        request = await read_frame(reader)
        writer.write(encode_frame(
            make_result(request.message["id"], {"format": "png"}), png
        ))
        await writer.drain()

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            response = await client.call_frame("screenshot", {})
            assert response.result == {"format": "png"}
            assert response.binary == png
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_out_of_order_responses_are_matched_by_id():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        first = await read_frame(reader)
        second = await read_frame(reader)
        await _send(writer, make_result(second.message["id"], {"tag": "second"}))
        await _send(writer, make_result(first.message["id"], {"tag": "first"}))

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            first, second = await asyncio.gather(
                client.call("one"),
                client.call("two"),
            )
            assert first == {"tag": "first"}
            assert second == {"tag": "second"}
        finally:
            await client.close()


@pytest.mark.asyncio
async def test_connection_loss_fails_pending_call():
    async def handle(reader, writer):
        hello = await read_frame(reader)
        await _send(writer, make_result(hello.message["id"], HELLO))
        await read_frame(reader)
        writer.close()
        await writer.wait_closed()

    async with _server(handle) as port:
        client = await RpcClient.connect("127.0.0.1", port, "token")
        try:
            with pytest.raises(ClientClosed):
                await client.call("query", timeout=2.0)
        finally:
            await client.close()

