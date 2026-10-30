"""Unit tests for the protocol frame codec and envelopes."""
from __future__ import annotations

import asyncio
import json
import struct

import pytest

from qt_commander.protocol import (
    HEADER_SIZE,
    MAGIC,
    MAX_FRAME_BYTES,
    PeerClosed,
    ProtocolError,
    encode_frame,
    make_error,
    make_request,
    make_result,
    read_frame,
)


async def _read(data: bytes, *, max_frame: int = MAX_FRAME_BYTES):
    reader = asyncio.StreamReader()
    reader.feed_data(data)
    reader.feed_eof()
    return await read_frame(reader, max_frame=max_frame)


def test_roundtrip_without_binary():
    frame = encode_frame({"id": 1, "method": "ping"})
    magic, json_len, bin_len = struct.unpack("!III", frame[:HEADER_SIZE])
    assert magic == MAGIC
    assert bin_len == 0
    assert json_len == len(frame) - HEADER_SIZE
    parsed = asyncio.run(_read(frame))
    assert parsed.message == {"id": 1, "method": "ping"}
    assert parsed.binary == b""


def test_roundtrip_with_binary_and_utf8():
    message = {"id": 7, "result": {"text": "保存", "count": 2}}
    binary = b"\x89PNG\r\n\x1a\n"
    parsed = asyncio.run(_read(encode_frame(message, binary)))
    assert parsed.message == message
    assert parsed.binary == binary
    assert "保存".encode("utf-8") in encode_frame(message, binary)


def test_bad_magic():
    data = struct.pack("!III", 0, 2, 0) + b"{}"
    with pytest.raises(ProtocolError, match="bad frame magic"):
        asyncio.run(_read(data))


def test_empty_json_payload():
    data = struct.pack("!III", MAGIC, 0, 0)
    with pytest.raises(ProtocolError, match="empty JSON payload"):
        asyncio.run(_read(data))


def test_empty_eof_is_peer_closed():
    with pytest.raises(PeerClosed, match="closed by peer"):
        asyncio.run(_read(b""))


def test_truncated_header():
    with pytest.raises(ProtocolError, match="truncated frame header"):
        asyncio.run(_read(b"\x00\x00"))


def test_truncated_payload():
    data = struct.pack("!III", MAGIC, 10, 0) + b"{}"
    with pytest.raises(ProtocolError, match="truncated frame payload"):
        asyncio.run(_read(data))


def test_max_frame_is_enforced():
    data = encode_frame({"id": 1})
    with pytest.raises(ProtocolError, match="frame too large"):
        asyncio.run(_read(data, max_frame=HEADER_SIZE))


def test_nan_is_rejected():
    with pytest.raises(ProtocolError, match="serializable"):
        encode_frame({"value": float("nan")})


def test_oversized_frame_is_rejected_before_payload_read():
    data = struct.pack("!III", MAGIC, MAX_FRAME_BYTES, 0)
    with pytest.raises(ProtocolError, match="frame too large"):
        asyncio.run(_read(data))


def test_non_object_json_is_rejected():
    data = struct.pack("!III", MAGIC, 2, 0) + b"[]"
    with pytest.raises(ProtocolError, match="must be an object"):
        asyncio.run(_read(data))


def test_encode_rejects_oversized_and_non_mapping():
    with pytest.raises(ProtocolError, match="frame too large"):
        encode_frame({"id": 1}, b"x" * MAX_FRAME_BYTES)
    with pytest.raises(ProtocolError):
        encode_frame(["not", "an", "object"])  # type: ignore[arg-type]


def test_make_request_omits_unset_optional_fields():
    assert make_request(1, "ping") == {"id": 1, "method": "ping"}
    assert make_request(2, "query", {"limit": 1}, timeout_ms=500, op_id="x") == {
        "id": 2,
        "method": "query",
        "params": {"limit": 1},
        "timeout_ms": 500,
        "op_id": "x",
    }


def test_make_error_shape():
    error = make_error(
        9,
        "E_NOT_FOUND",
        "no match",
        cause="no_match",
        retryable=True,
        hint="try text_contains",
        details={"count": 0},
    )
    assert error == {
        "id": 9,
        "error": {
            "code": "E_NOT_FOUND",
            "message": "no match",
            "cause": "no_match",
            "retryable": True,
            "hint": "try text_contains",
            "details": {"count": 0},
        },
    }
    assert json.loads(json.dumps(error)) == error


def test_make_result_maps_to_dict():
    assert make_result(3, {"count": 1}) == {"id": 3, "result": {"count": 1}}
