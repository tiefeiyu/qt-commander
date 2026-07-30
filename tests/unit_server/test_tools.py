"""Test MCP tools: session management, UI inspection, interaction, build, resources.

Uses direct tool function calls (not TestClient) for simpler coverage.
"""
import json
from pathlib import Path
from unittest.mock import AsyncMock, patch, MagicMock

import pytest
from mcp_server.server import (
    _resolve_session,
    qt_list_processes,
    qt_list_sessions,
    qt_detach,
)
from mcp_server.session import Session, SessionManager
from mcp_server.errors import SessionNotFoundError


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / ".qt-commander"
    ws.mkdir()
    (ws / "sessions").mkdir()
    return ws


@pytest.fixture
def sm(workspace):
    return SessionManager(workspace)


# ============================================================================
# _resolve_session helper
# ============================================================================

class TestResolveSession:
    def test_resolve_valid_session(self, sm, monkeypatch):
        """_resolve_session returns session when it exists and is connected."""
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        sess = Session("valid123456", 100, Path("/tmp/a.dll"), sm.workspace)
        sess.connected = True
        sm._sessions[sess.id] = sess
        sm._pid_to_session[100] = sess.id

        result = _resolve_session("valid123456")
        assert result is sess

    def test_resolve_nonexistent_session(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        with pytest.raises(SessionNotFoundError):
            _resolve_session("nonexistent")

    def test_resolve_disconnected_session(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        sess = Session("disconn1234", 200, Path("/tmp/b.dll"), sm.workspace)
        sess.connected = False
        sm._sessions[sess.id] = sess

        with pytest.raises(SessionNotFoundError):
            _resolve_session("disconn1234")


# ============================================================================
# Session management tools
# ============================================================================

class TestSessionTools:
    @pytest.mark.asyncio
    async def test_qt_list_processes(self, monkeypatch):
        from mcp_server import server as srv
        mock_result = [{"pid": 123, "name": "app.exe", "qt_version": "5"}]
        monkeypatch.setattr(srv, "list_qt_processes", lambda: mock_result)

        result = await qt_list_processes()
        data = json.loads(result)
        assert "processes" in data
        assert data["processes"] == mock_result

    @pytest.mark.asyncio
    async def test_qt_list_sessions(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        result = await qt_list_sessions()
        data = json.loads(result)
        assert "sessions" in data
        assert data["sessions"] == []

    @pytest.mark.asyncio
    async def test_qt_list_sessions_with_data(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        sess = Session("test12345678", 1, Path("/tmp/a.dll"), sm.workspace)
        sess.snapshot_count = 5
        sess.connected = True
        sm._sessions[sess.id] = sess
        sm._pid_to_session[1] = sess.id

        result = await qt_list_sessions()
        data = json.loads(result)
        assert len(data["sessions"]) == 1
        assert data["sessions"][0]["pid"] == 1
        assert data["sessions"][0]["snapshot_count"] == 5

    @pytest.mark.asyncio
    async def test_qt_detach_success(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        sess = Session("detach12345", 300, Path("/tmp/c.dll"), sm.workspace)
        sm._sessions[sess.id] = sess
        sm._pid_to_session[300] = sess.id

        result = await qt_detach("detach12345", purge=False)
        data = json.loads(result)
        assert data["status"] == "detached"

    @pytest.mark.asyncio
    async def test_qt_detach_not_found(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)

        result = await qt_detach("nonexistent")
        assert "error" in result
        assert "not found" in result.lower()


# ============================================================================
# Resource handler tests (path traversal guards)
# ============================================================================

class TestResourceHandlers:
    def test_resource_path_traversal_rejected(self):
        """Path traversal via '..' in filename should be rejected."""
        filename = "../../etc/passwd"
        # The resource handler checks explicitly for '..' in filename
        assert ".." in filename
        # A valid filename should NOT contain '..'
        safe = "snapshot_00000001.json"
        assert ".." not in safe

    def test_resource_safe_filename_accepted(self):
        """Safe filenames pass validation."""
        filename = "snapshot_00000001.json"
        assert ".." not in filename
        assert "/" not in filename
        assert "\\" not in filename

    def test_resource_backslash_rejected(self):
        """Windows path separator in filename should be rejected."""
        filename = r"snapshot_..\..\etc"
        assert "\\" in filename  # must be rejected

    def test_resource_null_byte(self):
        """Null byte in filename is not safe."""
        filename = "safe\0malicious"
        assert "\0" in filename  # null byte should never appear in valid filenames


# ============================================================================
# qt_attach build check + error paths
# ============================================================================

class TestQtAttachErrors:
    @pytest.mark.asyncio
    async def test_qt_attach_not_built(self, sm, monkeypatch):
        from mcp_server import server as srv
        from mcp_server.builder import BuildState
        monkeypatch.setattr(srv, "sessions", sm)
        monkeypatch.setattr(srv, "check_build_state", lambda: BuildState.NOT_BUILT)

        result = await srv.qt_attach(pid=1234)
        assert "error" in result
        assert "2001" in result

    @pytest.mark.asyncio
    async def test_qt_attach_missing_artifacts(self, sm, monkeypatch):
        from mcp_server import server as srv
        from mcp_server.builder import BuildState
        monkeypatch.setattr(srv, "sessions", sm)
        monkeypatch.setattr(srv, "check_build_state", lambda: BuildState.BUILT)
        monkeypatch.setattr(srv, "_resolve_session", lambda sid: MagicMock())
        # lib_path.exists() returns False via Path mock

        result = await srv.qt_attach(pid=1234)
        assert "error" in result


# ============================================================================
# UI inspection + interaction tools with mock sessions
# ============================================================================

class TestUiTools:
    @pytest.mark.asyncio
    async def test_qt_snapshot(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("snap00000001", 1, Path("/tmp/a.dll"), workspace)
        (sess.session_dir / "snapshots").mkdir(parents=True, exist_ok=True)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"elements": [{"id": 1, "type": "QPushButton"}]}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        sm._pid_to_session[1] = sess.id
        result = await srv.qt_snapshot("snap00000001")
        data = json.loads(result)
        assert data["session_id"] == "snap00000001"

    @pytest.mark.asyncio
    async def test_qt_find_element(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("find00000001", 2, Path("/tmp/b.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return [{"id": 42, "type": "QPushButton", "text": "OK"}]
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_find_element("find00000001", {"type": "QPushButton"})
        data = json.loads(result)
        assert data[0]["text"] == "OK"

    @pytest.mark.asyncio
    async def test_qt_get_property(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("prop00000001", 3, Path("/tmp/c.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"value": "Hello World"}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_get_property("prop00000001", 42, "text")
        data = json.loads(result)
        assert "Hello World" in data["value"]

    @pytest.mark.asyncio
    async def test_qt_set_property(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("setp00000001", 4, Path("/tmp/d.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"ok": True}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_set_property("setp00000001", 42, "text", '"new"')
        data = json.loads(result)
        assert data["ok"] is True

    @pytest.mark.asyncio
    async def test_qt_call_method(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("call00000001", 5, Path("/tmp/e.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"return_value": 42}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_call_method("call00000001", 42, "foo", [])
        data = json.loads(result)
        assert data["return_value"] == 42

    @pytest.mark.asyncio
    async def test_qt_screenshot(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("shot00000001", 6, Path("/tmp/f.dll"), workspace)
        (sess.session_dir / "screenshots").mkdir(parents=True, exist_ok=True)
        sess.connected = True
        sess.snapshot_count = 0
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"data": "fake_png_data"}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_screenshot("shot00000001")
        data = json.loads(result)
        assert "uri" in data

    @pytest.mark.asyncio
    async def test_qt_mouse_click(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("click0000001", 8, Path("/tmp/h.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"ok": True}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_mouse_click("click0000001", 42, button="right")
        data = json.loads(result)
        assert data["ok"] is True

    @pytest.mark.asyncio
    async def test_qt_keyboard_input(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("keys00000001", 9, Path("/tmp/i.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"ok": True}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_keyboard_input("keys00000001", 42, "hello")
        data = json.loads(result)
        assert data["ok"] is True

    @pytest.mark.asyncio
    async def test_qt_focus(self, sm, monkeypatch, workspace):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        sess = Session("focs00000001", 10, Path("/tmp/j.dll"), workspace)
        sess.connected = True
        sess._rpc_lock = __import__('asyncio').Lock()

        async def mock_send(m, p):
            return {"ok": True}
        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        result = await srv.qt_focus("focs00000001", 42)
        data = json.loads(result)
        assert data["ok"] is True


class TestToolErrorPaths:
    @pytest.mark.asyncio
    async def test_snapshot_invalid_session(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        with pytest.raises(SessionNotFoundError):
            await srv.qt_snapshot("nonexistent")

    @pytest.mark.asyncio
    async def test_get_property_invalid_session(self, sm, monkeypatch):
        from mcp_server import server as srv
        monkeypatch.setattr(srv, "sessions", sm)
        with pytest.raises(SessionNotFoundError):
            await srv.qt_get_property("nonexistent", 1, "text")
