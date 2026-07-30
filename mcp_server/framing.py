"""Frame protocol: 4-byte big-endian length prefix + JSON-RPC payload.

Matches the C++ framing.h format exactly.
"""
import asyncio
import struct

MAX_FRAME_PAYLOAD = 16 * 1024 * 1024  # 16 MB


class FrameWriter:
    """Writes length-prefixed frames to an asyncio StreamWriter."""

    def __init__(self, writer: asyncio.StreamWriter):
        self._writer = writer

    async def write_frame(self, payload: bytes) -> None:
        if len(payload) == 0 or len(payload) > MAX_FRAME_PAYLOAD:
            raise ValueError(
                f"Invalid payload length: {len(payload)} (max {MAX_FRAME_PAYLOAD})"
            )
        header = struct.pack("!I", len(payload))
        self._writer.write(header + payload)
        await self._writer.drain()


class FrameReader:
    """Reads length-prefixed frames from an asyncio StreamReader.

    Uses readexactly() for simple, correct streaming decoding.
    """

    def __init__(self, reader: asyncio.StreamReader):
        self._reader = reader

    async def read_frame(self) -> bytes:
        header = await self._reader.readexactly(4)
        length = struct.unpack("!I", header)[0]
        if length == 0 or length > MAX_FRAME_PAYLOAD:
            raise ProtocolError(
                f"Invalid frame length: {length} (max {MAX_FRAME_PAYLOAD})"
            )
        return await self._reader.readexactly(length)


class ProtocolError(Exception):
    """Raised when the frame protocol is violated."""
