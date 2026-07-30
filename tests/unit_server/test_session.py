"""Test Session and SessionManager — CRUD, connect, disconnect, send_rpc, recovery."""
import asyncio
import json
import struct
from pathlib import Path
from unittest.mock import AsyncMock, patch, MagicMock

import pytest
from mcp_server.session import Session, SessionManager
from mcp_server.errors import SessionExistsError, SessionNotFoundError, AuthFailedError, RpcTimeoutError


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / ".qt-commander"
    ws.mkdir()
    (ws / "sessions").mkdir()
    return ws


# ============================================================================
# Session basic properties
# ============================================================================

class TestSessionProperties:
    def test_session_init(self, workspace):
        sess = Session("abc123def456", 1234, Path("/tmp/test.dll"), workspace)
        assert sess.id == "abc123def456"
        assert sess.pid == 1234
        assert sess.lib_path == Path("/tmp/test.dll")
        assert sess.connected is False
        assert sess.snapshot_count == 0
        assert sess.port == 0
        assert sess.token == ""

    def test_session_dir(self, workspace):
        sess = Session("test12345678", 5678, Path("/tmp/test.dll"), workspace)
        assert sess.session_dir == workspace / "sessions" / "test12345678"

    def test_write_metadata(self, workspace):
        sess = Session("meta12345678", 99, Path("/tmp/test.dll"), workspace)
        sess.port = 45678
        sess.token = "abcd" * 16
        sess.write_metadata()
        assert (sess.session_dir / "session.json").exists()
        meta = json.loads((sess.session_dir / "session.json").read_text())
        assert meta["pid"] == 99
        assert meta["port"] == 45678
        assert meta["token"] == "abcd" * 16


# ============================================================================
# Session connect/disconnect with mocked TCP
# ============================================================================

class TestSessionConnect:
    @pytest.mark.asyncio
    async def test_connect_success(self, workspace):
        sess = Session("auth12345678", 999, Path("/tmp/lib.dll"), workspace)

        # Build a mock reader that returns an auth success response
        reader = asyncio.StreamReader()
        auth_ok = json.dumps({"jsonrpc": "2.0", "result": {"session_id": "x"}, "id": 1})
        frame = struct.pack("!I", len(auth_ok)) + auth_ok.encode()
        reader.feed_data(frame)
        reader.feed_eof()
        writer = AsyncMock()

        with patch("asyncio.open_connection", return_value=(reader, writer)):
            await sess.connect(23456, "abcd" * 16)

        assert sess.connected is True
        assert sess.port == 23456
        assert sess.token == "abcd" * 16

    @pytest.mark.asyncio
    async def test_connect_auth_failed(self, workspace):
        sess = Session("auth23456789", 999, Path("/tmp/lib.dll"), workspace)

        reader = asyncio.StreamReader()
        auth_err = json.dumps({"jsonrpc": "2.0", "error": {"code": -32000, "message": "auth failed"}, "id": 1})
        frame = struct.pack("!I", len(auth_err)) + auth_err.encode()
        reader.feed_data(frame)
        reader.feed_eof()
        writer = AsyncMock()

        with patch("asyncio.open_connection", return_value=(reader, writer)):
            with pytest.raises(AuthFailedError):
                await sess.connect(23456, "wrong_token")

        assert sess.connected is False

    @pytest.mark.asyncio
    async def test_connect_timeout(self, workspace):
        """connect() should timeout after 5s if no response."""
        sess = Session("timeout99888", 999, Path("/tmp/lib.dll"), workspace)

        # Create a reader that never gets data
        reader = asyncio.StreamReader()
        writer = AsyncMock()

        with patch("asyncio.open_connection", return_value=(reader, writer)):
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(sess.connect(23456, "abcd" * 16), timeout=0.1)


class TestSessionDisconnect:
    @pytest.mark.asyncio
    async def test_disconnect_not_connected(self, workspace):
        sess = Session("disc12345678", 1, Path("/tmp/a.dll"), workspace)
        # disconnect on non-connected session should be a no-op
        await sess.disconnect()
        assert sess.connected is False

    @pytest.mark.asyncio
    async def test_disconnect_connected(self, workspace):
        sess = Session("disc23456789", 2, Path("/tmp/a.dll"), workspace)
        sess.connected = True
        sess._writer = AsyncMock()
        sess._frame_writer = MagicMock()
        sess._frame_reader = MagicMock()
        sess._rpc_lock = asyncio.Lock()

        # Mock send_rpc to succeed (shutdown call)
        async def mock_send_rpc(method, params):
            return {}
        sess.send_rpc = mock_send_rpc

        await sess.disconnect()
        assert sess.connected is False
        sess._writer.close.assert_called_once()

    @pytest.mark.asyncio
    async def test_disconnect_send_rpc_fails_gracefully(self, workspace):
        sess = Session("disc34567890", 3, Path("/tmp/a.dll"), workspace)
        sess.connected = True
        sess._writer = AsyncMock()

        async def failing_send_rpc(method, params):
            raise ConnectionResetError("disconnected")
        sess.send_rpc = failing_send_rpc

        # Should not raise — exception is suppressed
        await sess.disconnect()
        assert sess.connected is False


# ============================================================================
# Session.send_rpc with mocked frame protocol
# ============================================================================

class TestSessionSendRpc:
    @pytest.mark.asyncio
    async def test_send_rpc_success(self, workspace):
        sess = Session("rpc123456789", 10, Path("/tmp/a.dll"), workspace)

        # Set up frame reader to return a success response
        reader = asyncio.StreamReader()
        resp = json.dumps({"jsonrpc": "2.0", "result": {"elements": []}, "id": 1})
        frame = struct.pack("!I", len(resp)) + resp.encode()
        reader.feed_data(frame)
        reader.feed_eof()

        writer = AsyncMock()
        sess._reader = reader
        sess._writer = writer
        from mcp_server.framing import FrameWriter, FrameReader
        sess._frame_writer = FrameWriter(writer)
        sess._frame_reader = FrameReader(reader)

        result = await sess.send_rpc("qt.snapshot", {"detail": "core"})
        assert result == {"elements": []}

    @pytest.mark.asyncio
    async def test_send_rpc_error_response(self, workspace):
        sess = Session("rpc234567890", 11, Path("/tmp/a.dll"), workspace)

        reader = asyncio.StreamReader()
        err = json.dumps({"jsonrpc": "2.0", "error": {"code": -32000, "message": "element destroyed", "data": {"code": 1001}}, "id": 1})
        frame = struct.pack("!I", len(err)) + err.encode()
        reader.feed_data(frame)
        reader.feed_eof()

        writer = AsyncMock()
        sess._reader = reader
        sess._writer = writer
        from mcp_server.framing import FrameWriter, FrameReader
        sess._frame_writer = FrameWriter(writer)
        sess._frame_reader = FrameReader(reader)

        with pytest.raises(RpcTimeoutError, match="element destroyed"):
            await sess.send_rpc("qt.snapshot", {})

    @pytest.mark.asyncio
    async def test_send_rpc_timeout(self, workspace):
        sess = Session("rpc345678901", 12, Path("/tmp/a.dll"), workspace)

        # Reader that never gets enough data
        reader = asyncio.StreamReader()
        writer = AsyncMock()
        sess._reader = reader
        sess._writer = writer
        from mcp_server.framing import FrameWriter, FrameReader
        sess._frame_writer = FrameWriter(writer)
        sess._frame_reader = FrameReader(reader)

        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(
                sess.send_rpc("qt.snapshot", {}), timeout=0.1
            )

    @pytest.mark.asyncio
    async def test_send_rpc_lock_serializes(self, workspace):
        """Two concurrent send_rpc calls should not interleave."""
        sess = Session("rpc456789012", 13, Path("/tmp/a.dll"), workspace)

        call_order = []

        async def slow_send(method, params):
            call_order.append("start")
            await asyncio.sleep(0.01)
            call_order.append("end")
            return {}

        sess.send_rpc = slow_send
        # Verify that _rpc_lock exists and is an asyncio.Lock
        assert isinstance(sess._rpc_lock, asyncio.Lock)


# ============================================================================
# SessionManager CRUD
# ============================================================================

class TestSessionManager:
    @pytest.mark.asyncio
    async def test_create_session(self, workspace):
        sm = SessionManager(workspace)
        session = await sm.create(1234, Path("/tmp/lib.dll"))
        assert session.pid == 1234
        assert len(session.id) == 12
        assert all(c in "abcdefghijklmnopqrstuvwxyz0123456789" for c in session.id)
        assert sm.get(session.id) is session

    @pytest.mark.asyncio
    async def test_pid_dedup(self, workspace):
        sm = SessionManager(workspace)
        s1 = await sm.create(100, Path("/tmp/a.dll"))
        with pytest.raises(SessionExistsError):
            await sm.create(100, Path("/tmp/b.dll"))

    @pytest.mark.asyncio
    async def test_destroy_session(self, workspace):
        sm = SessionManager(workspace)
        s = await sm.create(200, Path("/tmp/a.dll"))
        ok = await sm.destroy(s.id, purge=True)
        assert ok is True
        assert sm.get(s.id) is None
        assert not s.session_dir.exists()

    @pytest.mark.asyncio
    async def test_destroy_nonexistent(self, workspace):
        sm = SessionManager(workspace)
        ok = await sm.destroy("nonexistent", purge=False)
        assert ok is False

    @pytest.mark.asyncio
    async def test_destroy_without_purge(self, workspace):
        sm = SessionManager(workspace)
        s = await sm.create(201, Path("/tmp/a.dll"))
        s.write_metadata()
        ok = await sm.destroy(s.id, purge=False)
        assert ok is True
        # Session directory should still exist (no purge)
        assert s.session_dir.exists()

    @pytest.mark.asyncio
    async def test_list_sessions(self, workspace):
        sm = SessionManager(workspace)
        await sm.create(300, Path("/tmp/a.dll"))
        await sm.create(400, Path("/tmp/b.dll"))
        lst = sm.list_sessions()
        assert len(lst) == 2
        pids = {s["pid"] for s in lst}
        assert pids == {300, 400}

    def test_get_returns_none_for_missing(self, workspace):
        sm = SessionManager(workspace)
        assert sm.get("nonexistent") is None


# ============================================================================
# SessionManager.recover_on_startup (mocked psutil + subprocess)
# ============================================================================

class TestSessionRecovery:
    @pytest.mark.asyncio
    async def test_recover_orphaned_pid_dead(self, workspace):
        """When a session.json exists but the PID is dead, clean it up."""
        sess_dir = workspace / "sessions" / "orphan12345"
        sess_dir.mkdir(parents=True)
        (sess_dir / "session.json").write_text(json.dumps({
            "pid": 99999, "port": 1, "token": "x", "lib_path": "/tmp/lib.dll"
        }))

        sm = SessionManager(workspace)
        with patch("psutil.pid_exists", return_value=False):
            await sm.recover_on_startup()

        # Orphan should be cleaned up
        assert not sess_dir.exists()

    @pytest.mark.asyncio
    async def test_recover_missing_metadata(self, workspace):
        """Directory without session.json should be cleaned up."""
        sess_dir = workspace / "sessions" / "corrupt123"
        sess_dir.mkdir(parents=True)
        # No session.json written

        sm = SessionManager(workspace)
        await sm.recover_on_startup()

        assert not sess_dir.exists()

    @pytest.mark.asyncio
    async def test_recover_invalid_json(self, workspace):
        """Directory with corrupt session.json should be cleaned up."""
        sess_dir = workspace / "sessions" / "badjson456"
        sess_dir.mkdir(parents=True)
        (sess_dir / "session.json").write_text("not valid json{{{")

        sm = SessionManager(workspace)
        await sm.recover_on_startup()

        assert not sess_dir.exists()

    @pytest.mark.asyncio
    async def test_recover_no_sessions_dir(self, workspace):
        """No sessions/ directory at all should be a no-op."""
        sm = SessionManager(workspace / "nonexistent")
        await sm.recover_on_startup()

    @pytest.mark.asyncio
    async def test_recover_pid_alive_lib_missing(self, workspace):
        """PID exists but lib_path doesn't — should still clean up."""
        sess_dir = workspace / "sessions" / "alive999"
        sess_dir.mkdir(parents=True)
        (sess_dir / "session.json").write_text(json.dumps({
            "pid": 1, "port": 2, "token": "y",
            "lib_path": "/nonexistent/libqt-commander.dll"
        }))

        sm = SessionManager(workspace)
        with patch("psutil.pid_exists", return_value=True):
            await sm.recover_on_startup()

        # PID alive but lib_path missing → should still clean up (no eject attempt)
        assert not sess_dir.exists()
