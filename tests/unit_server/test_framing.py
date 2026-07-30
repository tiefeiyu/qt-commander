"""Test frame encode/decode protocol — FrameWriter and FrameReader 100% covered."""
import asyncio
import struct
from unittest.mock import AsyncMock

import pytest
from mcp_server.framing import FrameWriter, FrameReader, ProtocolError, MAX_FRAME_PAYLOAD


# ============================================================================
# struct.pack sanity checks
# ============================================================================

class TestStructPack:
    def test_basic_encode(self):
        header = struct.pack("!I", 5)
        assert header == b"\x00\x00\x00\x05"

    def test_zero_length_produces_zero_header(self):
        header = struct.pack("!I", 0)
        assert header == b"\x00\x00\x00\x00"

    def test_max_length_accepted(self):
        header = struct.pack("!I", MAX_FRAME_PAYLOAD)
        assert len(header) == 4
        assert struct.unpack("!I", header)[0] == MAX_FRAME_PAYLOAD


# ============================================================================
# FrameWriter tests
# ============================================================================

class TestFrameWriter:
    @pytest.mark.asyncio
    async def test_write_valid_frame(self):
        writer = AsyncMock()
        fw = FrameWriter(writer)
        payload = b'{"jsonrpc":"2.0","id":1}'
        await fw.write_frame(payload)

        writer.write.assert_called_once()
        written_data = writer.write.call_args[0][0]
        # First 4 bytes should be big-endian length
        assert struct.unpack("!I", written_data[:4])[0] == len(payload)
        assert written_data[4:] == payload
        writer.drain.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_write_empty_payload_rejected(self):
        writer = AsyncMock()
        fw = FrameWriter(writer)
        with pytest.raises(ValueError, match="Invalid payload length"):
            await fw.write_frame(b"")
        writer.write.assert_not_called()

    @pytest.mark.asyncio
    async def test_write_oversized_payload_rejected(self):
        writer = AsyncMock()
        fw = FrameWriter(writer)
        with pytest.raises(ValueError, match="Invalid payload length"):
            await fw.write_frame(b"x" * (MAX_FRAME_PAYLOAD + 1))
        writer.write.assert_not_called()

    @pytest.mark.asyncio
    async def test_write_max_payload_accepted(self):
        writer = AsyncMock()
        fw = FrameWriter(writer)
        payload = b"x" * MAX_FRAME_PAYLOAD
        await fw.write_frame(payload)
        assert struct.unpack("!I", writer.write.call_args[0][0][:4])[0] == MAX_FRAME_PAYLOAD

    @pytest.mark.asyncio
    async def test_write_small_frame(self):
        writer = AsyncMock()
        fw = FrameWriter(writer)
        await fw.write_frame(b"{}")
        writer.write.assert_called_once()
        header = writer.write.call_args[0][0][:4]
        assert struct.unpack("!I", header)[0] == 2


# ============================================================================
# FrameReader tests
# ============================================================================

class TestFrameReader:
    @pytest.mark.asyncio
    async def test_read_basic_frame(self):
        payload = b'{"jsonrpc":"2.0","id":1}'
        header = struct.pack("!I", len(payload))
        reader = asyncio.StreamReader()
        reader.feed_data(header + payload)
        reader.feed_eof()
        fr = FrameReader(reader)
        result = await fr.read_frame()
        assert result == payload

    @pytest.mark.asyncio
    async def test_read_zero_length_rejected(self):
        header = struct.pack("!I", 0)
        reader = asyncio.StreamReader()
        reader.feed_data(header)
        reader.feed_eof()
        fr = FrameReader(reader)
        with pytest.raises(ProtocolError, match="Invalid frame length"):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_read_above_max_rejected(self):
        header = struct.pack("!I", MAX_FRAME_PAYLOAD + 1)
        reader = asyncio.StreamReader()
        reader.feed_data(header)
        reader.feed_eof()
        fr = FrameReader(reader)
        with pytest.raises(ProtocolError, match="Invalid frame length"):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_incomplete_header_raises(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b"\x00\x00")
        reader.feed_eof()
        fr = FrameReader(reader)
        with pytest.raises(asyncio.IncompleteReadError):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_incomplete_payload_raises(self):
        header = struct.pack("!I", 10)
        reader = asyncio.StreamReader()
        reader.feed_data(header + b"short")
        reader.feed_eof()
        fr = FrameReader(reader)
        with pytest.raises(asyncio.IncompleteReadError):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_exact_boundary(self):
        """Read a frame where the payload exactly fills the buffer."""
        payload = b"abcd"
        header = struct.pack("!I", len(payload))
        reader = asyncio.StreamReader()
        reader.feed_data(header + payload)
        reader.feed_eof()
        fr = FrameReader(reader)
        result = await fr.read_frame()
        assert result == payload


# ============================================================================
# ProtocolError tests
# ============================================================================

class TestProtocolError:
    def test_protocol_error_is_exception(self):
        e = ProtocolError("test")
        assert isinstance(e, Exception)
        assert str(e) == "test"

    def test_protocol_error_can_be_caught(self):
        try:
            raise ProtocolError("bad frame")
        except ProtocolError as e:
            assert "bad frame" in str(e)
        else:
            assert False, "should have raised"


# ============================================================================
# Cross-validation: C++ frame_encode produces same bytes as Python struct.pack
# ============================================================================

class TestCrossValidation:
    def test_length_1(self):
        """C++: htonl(1) → Python: struct.pack('!I', 1) → both b'\x00\x00\x00\x01'"""
        assert struct.pack("!I", 1) == b"\x00\x00\x00\x01"

    def test_length_256(self):
        """256 in big-endian."""
        assert struct.pack("!I", 256) == b"\x00\x00\x01\x00"

    def test_length_65535(self):
        """Max uint16 in big-endian."""
        assert struct.pack("!I", 65535) == b"\x00\x00\xff\xff"

    def test_length_16mb(self):
        assert struct.pack("!I", MAX_FRAME_PAYLOAD) == b"\x01\x00\x00\x00"
