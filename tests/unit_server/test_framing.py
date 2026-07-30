"""Test frame encode/decode protocol."""
import asyncio
import struct
import pytest
from mcp_server.framing import ProtocolError, MAX_FRAME_PAYLOAD


class TestFrameEncode:
    def test_basic_encode(self):
        header = struct.pack("!I", 5)
        assert header == b"\x00\x00\x00\x05"

    def test_zero_length_produces_zero_header(self):
        header = struct.pack("!I", 0)
        assert header == b"\x00\x00\x00\x00"
        # struct.pack doesn't reject 0; FrameReader.read_frame() does

    def test_max_length_accepted(self):
        header = struct.pack("!I", MAX_FRAME_PAYLOAD)
        assert len(header) == 4
        assert struct.unpack("!I", header)[0] == MAX_FRAME_PAYLOAD


class TestFrameReader:
    @pytest.mark.asyncio
    async def test_read_basic_frame(self):
        payload = b'{"jsonrpc":"2.0","id":1}'
        header = struct.pack("!I", len(payload))
        reader = asyncio.StreamReader()
        reader.feed_data(header + payload)
        reader.feed_eof()
        from mcp_server.framing import FrameReader
        fr = FrameReader(reader)
        result = await fr.read_frame()
        assert result == payload

    @pytest.mark.asyncio
    async def test_read_zero_length_rejected(self):
        header = struct.pack("!I", 0)
        reader = asyncio.StreamReader()
        reader.feed_data(header)
        reader.feed_eof()
        from mcp_server.framing import FrameReader
        fr = FrameReader(reader)
        with pytest.raises(ProtocolError):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_read_above_max_rejected(self):
        header = struct.pack("!I", MAX_FRAME_PAYLOAD + 1)
        reader = asyncio.StreamReader()
        reader.feed_data(header)
        reader.feed_eof()
        from mcp_server.framing import FrameReader
        fr = FrameReader(reader)
        with pytest.raises(ProtocolError):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_incomplete_header_raises(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b"\x00\x00")
        reader.feed_eof()
        from mcp_server.framing import FrameReader
        fr = FrameReader(reader)
        with pytest.raises(asyncio.IncompleteReadError):
            await fr.read_frame()

    @pytest.mark.asyncio
    async def test_incomplete_payload_raises(self):
        header = struct.pack("!I", 10)
        reader = asyncio.StreamReader()
        reader.feed_data(header + b"short")
        reader.feed_eof()
        from mcp_server.framing import FrameReader
        fr = FrameReader(reader)
        with pytest.raises(asyncio.IncompleteReadError):
            await fr.read_frame()
