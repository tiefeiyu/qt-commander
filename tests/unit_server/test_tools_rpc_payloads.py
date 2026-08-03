"""Test the exact RPC wire payloads (method name + params) each MCP tool sends.

These are regression tests for known fix points:
- qt_find_element must forward the full query dict (method "qt.findElement")
- qt_keyboard_input must send "qt.typeText" (not a historical name)
- qt_call_method must send method/args
- qt_mouse_click must send "qt.click"
- qt_screenshot must pass dir and seq and read back the PNG

The existing test_tools.py covers return values; this file pins the
protocol payloads the injected library dispatches on.
"""
import asyncio
import json
from pathlib import Path

import pytest
from qt_commander.server import (
    qt_snapshot,
    qt_find_element,
    qt_get_property,
    qt_set_property,
    qt_call_method,
    qt_mouse_click,
    qt_keyboard_input,
    qt_focus,
    qt_attach,
    qt_screenshot,
)
from qt_commander.session import Session


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / ".qt-commander"
    ws.mkdir()
    (ws / "sessions").mkdir()
    return ws


@pytest.fixture
def sm(workspace):
    from qt_commander.session import SessionManager
    return SessionManager(workspace)


def make_session(sm, sid, pid, workspace):
    """Create a connected, mockable session wired into the manager."""
    sess = Session(sid, pid, Path("/tmp/a.dll"), workspace)
    sess.connected = True
    sess._rpc_lock = asyncio.Lock()
    captured = {"method": None, "params": None, "calls": []}

    async def mock_send(m, p):
        captured["method"] = m
        captured["params"] = p
        captured["calls"].append((m, p))
        return {"ok": True}

    sess.send_rpc = mock_send
    sm._sessions[sid] = sess
    sm._pid_to_session[pid] = sid
    return sess, captured


@pytest.fixture
def server_sessions(sm, monkeypatch):
    from qt_commander import server as srv
    monkeypatch.setattr(srv, "sessions", sm)
    return srv


# ============================================================================
# qt_snapshot — method name + all snapshot params
# ============================================================================

class TestSnapshotPayload:
    @pytest.mark.asyncio
    async def test_snapshot_sends_qt_snapshot_with_all_params(
        self, sm, workspace, server_sessions
    ):
        sess, captured = make_session(sm, "snap_p01", 1, workspace)
        (sess.session_dir / "snapshots").mkdir(parents=True, exist_ok=True)

        result = await qt_snapshot(
            "snap_p01",
            include_hidden=True,
            detail="core",
            root_id=7,
            max_depth=3,
            prop_depth=-1,
        )
        data = json.loads(result)

        # Method must match the library dispatch branch.
        assert captured["method"] == "qt.snapshot"
        params = captured["params"]
        assert params == {
            "include_hidden": True,
            "detail": "core",
            "rootId": 7,
            "maxDepth": 3,
            "propDepth": -1,
        }
        # Snapshot count increments and the JSON file is persisted.
        assert sess.snapshot_count == 1
        snap_file = sess.session_dir / "snapshots" / "snapshot_00000001.json"
        assert snap_file.exists()
        assert json.loads(snap_file.read_text()) == {"ok": True}
        assert data["snapshot_id"] == 1
        assert data["uri"] == (
            f"qt-commander://sessions/snap_p01/snapshots/snapshot_00000001.json"
        )

    @pytest.mark.asyncio
    async def test_snapshot_defaults(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "snap_p02", 2, workspace)
        (sess.session_dir / "snapshots").mkdir(parents=True, exist_ok=True)

        await qt_snapshot("snap_p02")
        assert captured["params"] == {
            "include_hidden": False,
            "detail": "extended",
            "rootId": 0,
            "maxDepth": 1,
            "propDepth": 1,
        }


# ============================================================================
# qt_find_element — regression: full query must be forwarded
# ============================================================================

class TestFindElementPayload:
    @pytest.mark.asyncio
    async def test_find_element_forwards_query(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "find_p01", 3, workspace)

        query = {"type": "QPushButton"}
        result = await qt_find_element("find_p01", query)
        data = json.loads(result)

        assert captured["method"] == "qt.findElement"
        # The query dict must be passed through verbatim, not flattened or dropped.
        assert captured["params"] == {"query": {"type": "QPushButton"}}
        assert data == {"ok": True}

    @pytest.mark.asyncio
    async def test_find_element_full_query_passthrough(self, sm, workspace, server_sessions):
        """Every supported query field survives the round trip unchanged."""
        sess, captured = make_session(sm, "find_p02", 4, workspace)

        query = {
            "type": "QLineEdit",
            "type_inherits": "QWidget",
            "text": "Search",
            "text_contains": "ear",
            "object_name": "searchBox",
            "window_title": "Main Window",
            "window_title_contains": "Main",
            "properties": {"enabled": True},
            "ancestor_id": 11,
            "window_id": 22,
        }
        await qt_find_element("find_p02", query)
        assert captured["params"] == {"query": query}


# ============================================================================
# qt_get_property / qt_set_property
# ============================================================================

class TestPropertyPayloads:
    @pytest.mark.asyncio
    async def test_get_property_sends_element_and_name(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "prop_p01", 5, workspace)

        await qt_get_property("prop_p01", 42, "text")
        assert captured["method"] == "qt.getProperty"
        assert captured["params"] == {"element_id": 42, "name": "text"}

    @pytest.mark.asyncio
    async def test_set_property_sends_parsed_json_value(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "prop_p02", 6, workspace)

        await qt_set_property("prop_p02", 42, "geometry", '{"x": 1, "y": 2}')
        assert captured["method"] == "qt.setProperty"
        assert captured["params"] == {
            "element_id": 42,
            "name": "geometry",
            "value": {"x": 1, "y": 2},
        }

    @pytest.mark.asyncio
    async def test_set_property_json_scalar_value(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "prop_p03", 7, workspace)

        await qt_set_property("prop_p03", 1, "maxLength", "5")
        assert captured["params"]["value"] == 5  # JSON number, not string


# ============================================================================
# qt_call_method — regression: method/args must be forwarded
# ============================================================================

class TestCallMethodPayload:
    @pytest.mark.asyncio
    async def test_call_method_sends_method_and_args(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "call_p01", 8, workspace)

        result = await qt_call_method("call_p01", 42, "setValue", [7, "x"])
        data = json.loads(result)

        assert captured["method"] == "qt.callMethod"
        assert captured["params"] == {
            "element_id": 42,
            "method": "setValue",
            "args": [7, "x"],
        }
        assert data == {"ok": True}

    @pytest.mark.asyncio
    async def test_call_method_no_args_sends_empty_list(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "call_p02", 9, workspace)

        # args omitted entirely (None) must serialize as [] on the wire.
        await qt_call_method("call_p02", 42, "show")
        assert captured["params"] == {"element_id": 42, "method": "show", "args": []}

    @pytest.mark.asyncio
    async def test_call_method_explicit_empty_args(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "call_p03", 10, workspace)

        await qt_call_method("call_p03", 1, "hide", [])
        assert captured["params"]["args"] == []


# ============================================================================
# qt_mouse_click — regression: "qt.click" + button + modifiers
# ============================================================================

class TestMouseClickPayload:
    @pytest.mark.asyncio
    async def test_click_sends_qt_click_with_modifiers(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "clck_p01", 11, workspace)

        result = await qt_mouse_click(
            "clck_p01", 42, button="right", modifiers=["Ctrl", "Shift"]
        )
        data = json.loads(result)

        # Must match the library's dispatch branch name, not "qt.mouseClick".
        assert captured["method"] == "qt.click"
        assert captured["params"] == {
            "element_id": 42,
            "button": "right",
            "modifiers": ["Ctrl", "Shift"],
        }
        assert data == {"ok": True}

    @pytest.mark.asyncio
    async def test_click_defaults_button_and_modifiers(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "clck_p02", 12, workspace)

        await qt_mouse_click("clck_p02", 42)
        assert captured["params"] == {
            "element_id": 42,
            "button": "left",
            "modifiers": [],
        }


# ============================================================================
# qt_keyboard_input — regression: "qt.typeText"
# ============================================================================

class TestKeyboardInputPayload:
    @pytest.mark.asyncio
    async def test_keyboard_input_sends_qt_type_text(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "keys_p01", 13, workspace)

        result = await qt_keyboard_input(
            "keys_p01", 42, "hello", modifiers=["Alt"]
        )
        data = json.loads(result)

        assert captured["method"] == "qt.typeText"
        assert captured["params"] == {
            "element_id": 42,
            "text": "hello",
            "modifiers": ["Alt"],
        }
        assert data == {"ok": True}

    @pytest.mark.asyncio
    async def test_keyboard_input_default_modifiers(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "keys_p02", 14, workspace)

        await qt_keyboard_input("keys_p02", 42, "abc")
        assert captured["params"]["modifiers"] == []


# ============================================================================
# qt_focus
# ============================================================================

class TestFocusPayload:
    @pytest.mark.asyncio
    async def test_focus_sends_qt_focus(self, sm, workspace, server_sessions):
        sess, captured = make_session(sm, "focs_p01", 15, workspace)

        result = await qt_focus("focs_p01", 42)
        data = json.loads(result)

        assert captured["method"] == "qt.focus"
        assert captured["params"] == {"element_id": 42}
        assert data == {"ok": True}


# ============================================================================
# qt_screenshot — failure path: keeps the error payload readable
# ============================================================================

class TestScreenshotFailurePath:
    @pytest.mark.asyncio
    async def test_screenshot_failure_writes_error_json(self, sm, workspace, server_sessions):
        sess = Session("shot_f01", 16, Path("/tmp/f.dll"), workspace)
        shot_dir = sess.session_dir / "screenshots"
        shot_dir.mkdir(parents=True, exist_ok=True)
        sess.connected = True
        sess._rpc_lock = asyncio.Lock()
        captured = {}

        async def mock_send(m, p):
            captured.update(p)
            return {"ok": False, "error": "element not visible"}

        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        sm._pid_to_session[sess.pid] = sess.id

        result = await qt_screenshot("shot_f01")
        data = json.loads(result)

        assert captured["dir"] == str(shot_dir)
        assert captured["seq"] == 1
        assert "uri" in data
        # The failure payload is written so the resource endpoint can serve it.
        error_file = shot_dir / "screenshot_00000001.png"
        assert error_file.exists()
        assert json.loads(error_file.read_text()) == {
            "ok": False, "error": "element not visible"
        }

    @pytest.mark.asyncio
    async def test_screenshot_missing_png_file(self, sm, workspace, server_sessions):
        """Library reports ok but the PNG path is unreadable -> error JSON."""
        sess = Session("shot_f02", 17, Path("/tmp/f.dll"), workspace)
        shot_dir = sess.session_dir / "screenshots"
        shot_dir.mkdir(parents=True, exist_ok=True)
        sess.connected = True
        sess._rpc_lock = asyncio.Lock()

        async def mock_send(m, p):
            return {"ok": True, "path": str(shot_dir / "missing_never_written.png")}

        sess.send_rpc = mock_send
        sm._sessions[sess.id] = sess
        sm._pid_to_session[sess.pid] = sess.id

        result = await qt_screenshot("shot_f02")
        data = json.loads(result)
        assert "error" in data
        assert "failed to read screenshot file" in data["error"]


# ============================================================================
# qt_attach — success path
# ============================================================================

class TestAttachSuccess:
    @pytest.mark.asyncio
    async def test_qt_attach_success(self, sm, workspace, server_sessions, monkeypatch):
        import os
        from qt_commander.builder import BuildState

        bin_dir = sm.workspace / "bin"
        bin_dir.mkdir(parents=True)
        lib_name = "libqt-commander.dll" if os.name == "nt" else "libqt-commander.so"
        inj_name = "qt-injector.exe" if os.name == "nt" else "qt-injector"
        (bin_dir / lib_name).write_text("fake")
        (bin_dir / inj_name).write_text("fake")

        attached = {}

        async def mock_inject(pid, lib_path, port_file, injector_exe, session):
            attached["pid"] = pid
            session.connected = True

        monkeypatch.setattr(server_sessions, "check_build_state",
                            lambda: BuildState.BUILT)
        monkeypatch.setattr(server_sessions, "BUILD_DIR", sm.workspace)
        monkeypatch.setattr(server_sessions, "inject_and_connect", mock_inject)

        result = await qt_attach(pid=1234)
        data = json.loads(result)

        assert data["pid"] == 1234
        assert data["connected"] is True
        assert "session_id" in data
        assert attached["pid"] == 1234
        # The session is registered with the manager.
        assert server_sessions.sessions.get(data["session_id"]) is not None
