"""Session lifecycle management for injected Qt processes."""
import asyncio
import json
import secrets
import shutil
import string
from datetime import datetime, timezone
from pathlib import Path

from .errors import (
    SessionNotFoundError,
    SessionExistsError,
    AuthFailedError,
    RpcTimeoutError,
    RpcError,
)
from .framing import FrameWriter, FrameReader


class Session:
    """A single injected Qt process session."""

    def __init__(self, sid: str, pid: int, lib_path: Path, workspace: Path):
        self.id = sid
        self.pid = pid
        self.lib_path = lib_path
        self.workspace = workspace
        self.port: int = 0
        self.token: str = ""
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._frame_writer: FrameWriter | None = None
        self._frame_reader: FrameReader | None = None
        self._rpc_lock = asyncio.Lock()
        self._request_id: int = 0
        self.snapshot_count: int = 0
        self.connected: bool = False

    @property
    def session_dir(self) -> Path:
        return self.workspace / "sessions" / self.id

    async def connect(self, port: int, token: str) -> None:
        """Establish TCP connection and authenticate."""
        self.port = port
        self.token = token
        self._reader, self._writer = await asyncio.open_connection("127.0.0.1", port)
        self._frame_writer = FrameWriter(self._writer)
        self._frame_reader = FrameReader(self._reader)

        self._request_id = 1
        auth_req = json.dumps({
            "jsonrpc": "2.0",
            "method": "qt.authenticate",
            "params": {"token": token},
            "id": self._request_id,
        })
        await self._frame_writer.write_frame(auth_req.encode("utf-8"))

        raw = await asyncio.wait_for(self._frame_reader.read_frame(), timeout=5.0)
        response = json.loads(raw.decode("utf-8"))
        if "error" in response:
            raise AuthFailedError()
        self.connected = True

    async def disconnect(self) -> None:
        """Send shutdown RPC and close TCP connection."""
        if not self.connected:
            return
        try:
            await self.send_rpc("qt.shutdown", {})
        except Exception:
            pass
        if self._writer:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except Exception:
                pass
        self.connected = False

    async def send_rpc(self, method: str, params: dict) -> dict:
        """Send a JSON-RPC request and await the response. Thread-safe via _rpc_lock."""
        async with self._rpc_lock:
            req_id = self._request_id
            self._request_id += 1
            request = json.dumps({
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
                "id": req_id,
            })
            assert self._frame_writer is not None
            assert self._frame_reader is not None
            await self._frame_writer.write_frame(request.encode("utf-8"))
            try:
                raw = await asyncio.wait_for(
                    self._frame_reader.read_frame(), timeout=30.0)
            except asyncio.TimeoutError:
                raise RpcTimeoutError(
                    "target process did not respond") from None
            response = json.loads(raw.decode("utf-8"))
            if "error" in response:
                err = response["error"]
                # Keep the library's code + message distinct (a JSON-RPC
                # error response is NOT a timeout); RpcError carries both.
                raise RpcError(
                    err.get("code", -32603),
                    err.get("message", "unknown RPC error"),
                )
            return response.get("result", {})

    def write_metadata(self) -> None:
        """Persist session metadata to session.json."""
        meta = {
            "pid": self.pid,
            "port": self.port,
            "token": self.token,
            "lib_path": str(self.lib_path),
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        self.session_dir.mkdir(parents=True, exist_ok=True)
        (self.session_dir / "session.json").write_text(json.dumps(meta))


class SessionManager:
    """Manages all active sessions. Thread-safe via _lock."""

    def __init__(self, workspace: Path):
        self.workspace = workspace
        self._sessions: dict[str, Session] = {}
        self._pid_to_session: dict[int, str] = {}
        self._lock = asyncio.Lock()

    def _generate_session_id(self) -> str:
        chars = string.ascii_lowercase + string.digits
        return "".join(secrets.choice(chars) for _ in range(12))

    async def create(self, pid: int, lib_path: Path) -> Session:
        async with self._lock:
            existing = self._pid_to_session.get(pid)
            if existing is not None:
                sess = self._sessions[existing]
                raise SessionExistsError(pid, sess.id)

            sid = self._generate_session_id()
            while sid in self._sessions:
                sid = self._generate_session_id()

            session = Session(sid, pid, lib_path, self.workspace)
            session.session_dir.mkdir(parents=True, exist_ok=True)
            (session.session_dir / "snapshots").mkdir(exist_ok=True)
            (session.session_dir / "screenshots").mkdir(exist_ok=True)

            self._sessions[sid] = session
            self._pid_to_session[pid] = sid
            return session

    async def destroy(self, session_id: str, purge: bool = False) -> bool:
        async with self._lock:
            session = self._sessions.pop(session_id, None)
            if session is None:
                return False
            self._pid_to_session.pop(session.pid, None)

            try:
                await session.disconnect()
            except Exception:
                pass

            if purge:
                # Unload the injected library from the target process first
                # so its DLL file is not locked (the build/cleanup steps
                # need to overwrite it).  Best-effort: a dead process or a
                # missing injector is fine.  The injector sits next to the
                # injected library in the build dir -- a bare "qt-injector"
                # would depend on PATH, which a uv-run server does not have.
                injector = session.lib_path.parent / "qt-injector.exe"
                try:
                    proc = await asyncio.create_subprocess_exec(
                        str(injector), "--eject", str(session.pid),
                        str(session.lib_path),
                        stdout=asyncio.subprocess.DEVNULL,
                        stderr=asyncio.subprocess.DEVNULL,
                    )
                    await asyncio.wait_for(proc.wait(), timeout=15.0)
                except Exception:
                    pass
                for attempt in range(3):
                    try:
                        if session.session_dir.exists():
                            shutil.rmtree(session.session_dir)
                        break
                    except PermissionError:
                        if attempt < 2:
                            await asyncio.sleep(2 ** attempt)
            return True

    def get(self, session_id: str) -> Session | None:
        return self._sessions.get(session_id)

    def list_sessions(self) -> list[dict]:
        return [
            {
                "session_id": s.id,
                "pid": s.pid,
                "connected": s.connected,
                "snapshot_count": s.snapshot_count,
            }
            for s in self._sessions.values()
        ]

    async def recover_on_startup(self) -> None:
        """Scan sessions/ for orphaned directories and recover or clean up."""
        sessions_root = self.workspace / "sessions"
        if not sessions_root.exists():
            return
        import psutil
        for session_dir in sessions_root.iterdir():
            meta_file = session_dir / "session.json"
            if not meta_file.exists():
                shutil.rmtree(session_dir, ignore_errors=True)
                continue
            try:
                meta = json.loads(meta_file.read_text())
            except json.JSONDecodeError:
                shutil.rmtree(session_dir, ignore_errors=True)
                continue
            pid = meta.get("pid", 0)
            if not psutil.pid_exists(pid):
                shutil.rmtree(session_dir, ignore_errors=True)
                continue
            lib_path = Path(meta.get("lib_path", ""))
            if lib_path.exists():
                injector = lib_path.parent / "qt-injector.exe"
                try:
                    proc = await asyncio.create_subprocess_exec(
                        str(injector), "--eject", str(pid), str(lib_path),
                        stdout=asyncio.subprocess.DEVNULL,
                        stderr=asyncio.subprocess.DEVNULL,
                    )
                    await asyncio.wait_for(proc.wait(), timeout=15.0)
                except Exception:
                    pass
            shutil.rmtree(session_dir, ignore_errors=True)
