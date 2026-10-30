"""Protocol frame codec and envelope helpers.

Frame layout (big-endian):

    [u32 magic][u32 json_len][u32 bin_len][utf-8 JSON][binary]

The magic is ``QTC2`` (0x51544332).  The binary segment is used for PNG/JPEG
payloads so images never go through base64.  A frame is capped at 64 MiB
(header + JSON + binary).
"""
from __future__ import annotations

import asyncio
import json
import struct
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

from .errors import ErrorCode, as_code

MAGIC = 0x51544332
MAX_FRAME_BYTES = 64 * 1024 * 1024
_HEADER = struct.Struct("!III")
HEADER_SIZE = _HEADER.size


class ProtocolError(Exception):
    """A malformed frame or JSON-RPC envelope."""

    def __init__(
        self, message: str, *, code: str | ErrorCode = ErrorCode.PROTOCOL
    ) -> None:
        super().__init__(message)
        self.code = as_code(code)


class PeerClosed(ProtocolError):
    """The peer closed cleanly at a frame boundary (not a protocol error)."""

    def __init__(self, message: str = "connection closed by peer") -> None:
        super().__init__(message, code=ErrorCode.PROTOCOL)


@dataclass(frozen=True, slots=True)
class Frame:
    message: dict[str, Any]
    binary: bytes = b""


def encode_frame(message: Mapping[str, Any], binary: bytes = b"") -> bytes:
    """Serialize one frame.

    ``message`` must be a JSON object.  Raises :class:`ProtocolError` when the
    frame is empty, oversized, or not serializable.
    """
    if not isinstance(message, Mapping):
        raise ProtocolError("message must be a JSON object")
    if not isinstance(binary, (bytes, bytearray, memoryview)):
        raise TypeError("binary must be bytes-like")
    binary = bytes(binary)
    try:
        json_bytes = json.dumps(
            message,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise ProtocolError(f"message is not JSON serializable: {exc}") from exc
    total = HEADER_SIZE + len(json_bytes) + len(binary)
    if total > MAX_FRAME_BYTES:
        raise ProtocolError(
            f"frame too large: {total} > {MAX_FRAME_BYTES}",
            code=ErrorCode.FRAME_TOO_LARGE,
        )
    return _HEADER.pack(MAGIC, len(json_bytes), len(binary)) + json_bytes + binary


async def read_frame(
    reader: asyncio.StreamReader, *, max_frame: int = MAX_FRAME_BYTES
) -> Frame:
    """Read exactly one frame from *reader*.

    Raises :class:`PeerClosed` only when the peer closes at a frame boundary;
    truncated headers/payloads and malformed JSON raise :class:`ProtocolError`.
    """
    if max_frame < HEADER_SIZE:
        raise ValueError("max_frame is smaller than the frame header")
    try:
        header = await reader.readexactly(HEADER_SIZE)
    except asyncio.IncompleteReadError as exc:
        if not exc.partial:
            raise PeerClosed() from exc
        raise ProtocolError("truncated frame header") from exc

    magic, json_len, bin_len = _HEADER.unpack(header)
    if magic != MAGIC:
        raise ProtocolError(f"bad frame magic: 0x{magic:08x}")
    if json_len == 0:
        raise ProtocolError("empty JSON payload")
    total = HEADER_SIZE + json_len + bin_len
    if total > max_frame:
        raise ProtocolError(
            f"frame too large: {total} > {max_frame}",
            code=ErrorCode.FRAME_TOO_LARGE,
        )

    try:
        json_bytes = await reader.readexactly(json_len)
        binary = await reader.readexactly(bin_len) if bin_len else b""
    except asyncio.IncompleteReadError as exc:
        raise ProtocolError("truncated frame payload") from exc

    try:
        message = json.loads(json_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid JSON payload: {exc}") from exc
    if not isinstance(message, dict):
        raise ProtocolError("JSON payload must be an object")
    return Frame(message=message, binary=binary)


def make_request(
    request_id: int,
    method: str,
    params: Mapping[str, Any] | None = None,
    *,
    timeout_ms: int | None = None,
    op_id: str | None = None,
) -> dict[str, Any]:
    """Build a request envelope; optional fields are omitted when unset."""
    if not isinstance(request_id, int) or isinstance(request_id, bool):
        raise TypeError("request_id must be an int")
    if not method:
        raise ValueError("method must not be empty")
    request: dict[str, Any] = {"id": request_id, "method": method}
    if params is not None:
        request["params"] = dict(params)
    if timeout_ms is not None:
        if timeout_ms < 0:
            raise ValueError("timeout_ms must be >= 0")
        request["timeout_ms"] = timeout_ms
    if op_id is not None:
        request["op_id"] = op_id
    return request


def make_result(request_id: int, result: Mapping[str, Any] | None = None) -> dict[str, Any]:
    if not isinstance(request_id, int) or isinstance(request_id, bool):
        raise TypeError("request_id must be an int")
    return {"id": request_id, "result": dict(result or {})}


def make_error(
    request_id: int,
    code: str | ErrorCode,
    message: str,
    *,
    cause: str | None = None,
    retryable: bool = False,
    hint: str | None = None,
    details: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build an error envelope; ``code`` is a stable ``E_*`` string."""
    wire_code = as_code(code)
    if not wire_code:
        raise ValueError("code must not be empty")
    error: dict[str, Any] = {
        "code": wire_code,
        "message": message,
        "retryable": bool(retryable),
    }
    if cause is not None:
        error["cause"] = cause
    if hint is not None:
        error["hint"] = hint
    if details is not None:
        error["details"] = dict(details)
    return {"id": request_id, "error": error}

