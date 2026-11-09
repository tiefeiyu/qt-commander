"""Session objects and the in-process session registry."""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any, Mapping

from .client import FrameResponse, RpcClient, ServerHello


class SessionNotFound(LookupError):
    """No live session with this id."""


class SessionExists(ValueError):
    """A session with this id is already registered."""


@dataclass(slots=True)
class Session:
    session_id: str
    pid: int
    client: RpcClient
    hello: ServerHello
    build_id: str | None = None
    disconnected_at: float | None = None
    grace_seconds: float = 60.0
    descriptor: str | None = None

    @property
    def connected(self) -> bool:
        if self.client.closed:
            if self.disconnected_at is None:
                self.disconnected_at = time.monotonic()
            return False
        return True

    @property
    def grace_remaining_ms(self) -> int | None:
        if self.connected:
            return 0
        if self.disconnected_at is None:
            return int(self.grace_seconds * 1000)
        elapsed = (time.monotonic() - self.disconnected_at) * 1000
        return max(0, int(self.grace_seconds * 1000 - elapsed))

    async def call(
        self,
        method: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        op_id: str | None = None,
    ) -> dict[str, Any]:
        return await self.client.call(
            method, params, timeout=timeout, op_id=op_id
        )

    async def call_frame(
        self,
        method: str,
        params: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        op_id: str | None = None,
    ) -> FrameResponse:
        return await self.client.call_frame(
            method, params, timeout=timeout, op_id=op_id
        )

    async def close(self) -> None:
        await self.client.close()
        if self.disconnected_at is None:
            self.disconnected_at = time.monotonic()


@dataclass(frozen=True, slots=True)
class SessionInfo:
    session_id: str
    pid: int
    library_version: str
    capabilities: tuple[str, ...]
    connected: bool
    build_id: str | None = None
    grace_remaining_ms: int | None = None


class SessionManager:
    """Single-event-loop registry; no background threads."""

    def __init__(self) -> None:
        self._sessions: dict[str, Session] = {}

    def add(self, session: Session) -> None:
        if session.session_id in self._sessions:
            raise SessionExists(session.session_id)
        self._sessions[session.session_id] = session

    def _evict_expired(self) -> None:
        for session_id, session in list(self._sessions.items()):
            if not session.connected and session.grace_remaining_ms == 0:
                self._sessions.pop(session_id, None)

    def get(self, session_id: str) -> Session:
        self._evict_expired()
        try:
            return self._sessions[session_id]
        except KeyError:
            raise SessionNotFound(session_id) from None

    def remove(self, session_id: str) -> Session:
        try:
            return self._sessions.pop(session_id)
        except KeyError:
            raise SessionNotFound(session_id) from None

    def list(self) -> list[SessionInfo]:
        self._evict_expired()
        return [
            SessionInfo(
                session_id=s.session_id,
                pid=s.pid,
                library_version=s.hello.library_version,
                capabilities=s.hello.capabilities,
                connected=s.connected,
                build_id=s.build_id,
                grace_remaining_ms=s.grace_remaining_ms,
            )
            for s in self._sessions.values()
        ]

    async def close_all(self) -> None:
        for session in list(self._sessions.values()):
            await session.close()
        self._sessions.clear()

