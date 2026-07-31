"""qt-commander MCP server — entry point."""
import json
import os
from pathlib import Path

from fastmcp import FastMCP

from .builder import BUILD_DIR, check_build_state, run_build, BuildState
from .errors import BuildRequiredError, SessionNotFoundError, tool_error
from .process_detector import list_qt_processes
from .rpc_client import inject_and_connect
from .session import SessionManager

mcp = FastMCP("qt-commander")

_workspace = Path(os.environ.get("QT_COMMANDER_WORKSPACE", ".qt-commander")).resolve()
_workspace.mkdir(parents=True, exist_ok=True)

sessions = SessionManager(_workspace)


def _resolve_session(session_id: str):
    sess = sessions.get(session_id)
    if sess is None or not sess.connected:
        raise SessionNotFoundError(session_id)
    return sess


# ============================================================================
# Session Management Tools
# ============================================================================

@mcp.tool()
async def qt_list_processes() -> str:
    """List running Qt processes that may be attachable."""
    procs = list_qt_processes()
    return json.dumps({"processes": procs}, indent=2)


@mcp.tool()
async def qt_attach(pid: int) -> str:
    """Inject the helper library into a running Qt process and open a session."""
    if check_build_state() != BuildState.BUILT:
        return tool_error(2001, (
            "Build required. Use qt_build first. Required params: vcvars_path, qt_env. "
            "Optional: vcvars_args, build_type, qt_major, generator."
        ))

    lib_path = BUILD_DIR / "library" / "build" / (
        "libqt-commander.dll" if os.name == "nt" else "libqt-commander.so"
    )
    injector_exe = BUILD_DIR / "injector" / "build" / (
        "qt-injector.exe" if os.name == "nt" else "qt-injector"
    )

    if not lib_path.exists() or not injector_exe.exists():
        return tool_error(2001, "Built artifacts not found. Re-run qt_build.")

    session = await sessions.create(pid, lib_path)
    port_file = session.session_dir / "port.txt"

    try:
        await inject_and_connect(pid, lib_path, port_file, injector_exe, session)
    except Exception as e:
        await sessions.destroy(session.id, purge=True)
        return tool_error(2002, str(e))

    return json.dumps({
        "session_id": session.id,
        "pid": pid,
        "connected": session.connected,
    })


@mcp.tool()
async def qt_detach(session_id: str, purge: bool = False) -> str:
    """Disconnect from a Qt process session. Optionally purge artifacts."""
    ok = await sessions.destroy(session_id, purge=purge)
    if not ok:
        return tool_error(-32602, f"Session not found: {session_id}")
    return json.dumps({"status": "detached", "session_id": session_id, "purged": purge})


@mcp.tool()
async def qt_list_sessions() -> str:
    """List all active sessions."""
    return json.dumps({"sessions": sessions.list_sessions()}, indent=2)


# ============================================================================
# Build Tool
# ============================================================================

@mcp.tool()
async def qt_build(
    vcvars_path: str,
    qt_env: str,
    vcvars_args: str = "",
    build_type: str = "Release",
    qt_major: int = 5,
    generator: str = "",
) -> str:
    """Build the Qt injection library and injector for the specified environment."""
    result = await run_build(
        vcvars_path=vcvars_path,
        qt_env=qt_env,
        vcvars_args=vcvars_args,
        build_type=build_type,
        qt_major=qt_major,
        generator=generator,
    )
    return json.dumps(result, indent=2)


# ============================================================================
# UI Inspection Tools
# ============================================================================

@mcp.tool()
async def qt_snapshot(session_id: str, include_hidden: bool = False, detail: str = "extended") -> str:
    """Take a full snapshot of the UI widget tree for the session."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.snapshot", {
        "include_hidden": include_hidden,
        "detail": detail,
    })
    session.snapshot_count += 1

    filename = f"snapshot_{session.snapshot_count:08d}.json"
    snap_path = session.session_dir / "snapshots" / filename
    snap_path.write_text(json.dumps(result, indent=2))

    return json.dumps({
        "session_id": session_id,
        "snapshot_id": session.snapshot_count,
        "uri": f"qt-commander://sessions/{session_id}/snapshots/{filename}",
    })


@mcp.tool()
async def qt_find_element(session_id: str, query: dict) -> str:
    """Find UI elements matching a query in a session."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.findElement", {"query": query})
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_get_property(session_id: str, element_id: int, name: str) -> str:
    """Read a property from a UI element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.getProperty", {
        "element_id": element_id, "name": name,
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_set_property(session_id: str, element_id: int, name: str, value: str) -> str:
    """Write a property value on a UI element."""
    session = _resolve_session(session_id)
    try:
        parsed_value = json.loads(value)
    except (json.JSONDecodeError, TypeError):
        parsed_value = value
    result = await session.send_rpc("qt.setProperty", {
        "element_id": element_id, "name": name, "value": parsed_value,
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_call_method(session_id: str, element_id: int, method: str, args: list | None = None) -> str:
    """Invoke a QMetaObject-invokable method on a UI element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.callMethod", {
        "element_id": element_id, "method": method, "args": args or [],
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_screenshot(session_id: str, element_id: int = 0) -> str:
    """Capture a screenshot of a UI element or the entire window."""
    session = _resolve_session(session_id)
    params = {}
    if element_id > 0:
        params["element_id"] = element_id
    result = await session.send_rpc("qt.screenshot", params)

    filename = f"screenshot_{session.snapshot_count + 1:08d}.png"
    ss_path = session.session_dir / "screenshots" / filename
    # Write raw PNG bytes, not JSON-encoded dict
    data = result.get("data", json.dumps(result)) if isinstance(result, dict) else result
    ss_path.write_bytes(data.encode() if isinstance(data, str) else data)

    return json.dumps({
        "session_id": session_id,
        "uri": f"qt-commander://sessions/{session_id}/screenshots/{filename}",
    })


# ============================================================================
# Interaction Tools
# ============================================================================

@mcp.tool()
async def qt_mouse_click(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None) -> str:
    """Send a mouse click to a UI element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.mouseClick", {
        "element_id": element_id, "button": button, "modifiers": modifiers or [],
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_keyboard_input(session_id: str, element_id: int, text: str, modifiers: list[str] | None = None) -> str:
    """Send keyboard input to a UI element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.typeText", {
        "element_id": element_id, "text": text, "modifiers": modifiers or [],
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_focus(session_id: str, element_id: int) -> str:
    """Set focus on a UI element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.focus", {"element_id": element_id})
    return json.dumps(result, indent=2)


# ============================================================================
# Resources
# ============================================================================

@mcp.resource("qt-commander://sessions/{session_id}/snapshots/{filename}")
async def read_snapshot_resource(session_id: str, filename: str) -> str:
    """Read a snapshot resource."""
    if ".." in filename or "/" in filename or "\\" in filename:
        return json.dumps({"error": "invalid filename"})
    sess = _resolve_session(session_id)
    path = (sess.session_dir / "snapshots" / filename).resolve()
    if not str(path).startswith(str(sess.session_dir.resolve())):
        return json.dumps({"error": "path traversal detected"})
    if not path.exists():
        return json.dumps({"error": "resource not found"})
    return path.read_text(encoding="utf-8")


@mcp.resource("qt-commander://sessions/{session_id}/screenshots/{filename}")
async def read_screenshot_resource(session_id: str, filename: str) -> bytes:
    """Read a screenshot resource."""
    if ".." in filename or "/" in filename or "\\" in filename:
        return b""
    sess = _resolve_session(session_id)
    path = (sess.session_dir / "screenshots" / filename).resolve()
    if not str(path).startswith(str(sess.session_dir.resolve())):
        return b""
    if not path.exists():
        return b""
    return path.read_bytes()


# ============================================================================
# Server lifecycle
# ============================================================================

def main():
    """Entry point for 'qt-commander-mcp' console script.

    Session recovery (sessions.recover_on_startup) is invoked automatically
    on first tool call via a lazy-init pattern, or can be triggered manually.
    """
    mcp.run()
