"""qt-commander MCP server — entry point."""
import json
import os
from pathlib import Path

from fastmcp import FastMCP

from .builder import BUILD_DIR, check_build_state, run_build, BuildState
from .environment_detector import detect_vs_environments, detect_qt_environments
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
    """Inject the helper library into a running Qt process and open a session.

    If the build has not been completed, follows the full workflow:
    qt_detect_msvc_and_qt → AskUserQuestion → qt_build → qt_attach.
    """
    if check_build_state() != BuildState.BUILT:
        return tool_error(2001, (
            "Build required. First, run qt_detect_msvc_and_qt to discover MSVC and Qt "
            "installations. Then ask the user which VS and Qt to use — do NOT auto-pick "
            "paths. Finally, call qt_build with the user-chosen vcvars_path and qt_env."
        ))

    lib_path = (BUILD_DIR / "bin" / (
        "libqt-commander.dll" if os.name == "nt" else "libqt-commander.so"
    )).resolve()
    injector_exe = (BUILD_DIR / "bin" / (
        "qt-injector.exe" if os.name == "nt" else "qt-injector"
    )).resolve()

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
# Build Tools
# ============================================================================

@mcp.tool()
async def qt_detect_msvc_and_qt() -> str:
    """Detect MSVC (Visual Studio) and Qt installations available for building.

    IMPORTANT — this tool only *discovers* and *displays* what is found on
    this machine.  It does NOT select an environment automatically.

    After calling this tool, the AI MUST use ``AskUserQuestion`` to present
    the found installations to the user and let them choose which VS
    (``vcvars_path``) and Qt (``qtenv_path``) to use with ``qt_build``.
    If detection finds nothing (or misses an installation the user knows
    about), the user can type the paths manually.

    Returns two lists: ``vs_installations`` and ``qt_installations``.
    """
    vs_list = detect_vs_environments()
    qt_list = detect_qt_environments()
    return json.dumps({
        "vs_installations": vs_list,
        "qt_installations": qt_list,
    }, indent=2)


@mcp.tool()
async def qt_build(
    vcvars_path: str,
    qt_env: str,
    vcvars_args: str = "",
    build_type: str = "Release",
    qt_major: int = 5,
    generator: str = "",
    with_qml: bool = True,
) -> str:
    """Build the Qt injection library and injector for the specified environment.

    Before calling this tool, the AI MUST present available build
    environments (from ``qt_detect_msvc_and_qt``) to the user and let them
    choose.  Do NOT guess or auto-pick paths — the user decides which VS
    and Qt installation to use.

    The AI MUST also ask the user whether to enable QML/QQuick support
    (``with_qml``).  QML support is required for inspecting QML/Qt Quick
    applications but adds build dependencies.

    The AI MUST also ask the user to choose the ``build_type`` (Debug or
    Release).  The build type must match the target process — a Debug
    process requires a Debug build, a Release process requires Release.
    """
    result = await run_build(
        vcvars_path=vcvars_path,
        qt_env=qt_env,
        vcvars_args=vcvars_args,
        build_type=build_type,
        qt_major=qt_major,
        generator=generator,
        with_qml=with_qml,
    )
    return json.dumps(result, indent=2)


# ============================================================================
# UI Inspection Tools
# ============================================================================

@mcp.tool()
async def qt_snapshot(session_id: str, include_hidden: bool = False,
                       detail: str = "extended",
                       root_id: int = 0, max_depth: int = 1,
                       prop_depth: int = 1) -> str:
    """Take a full snapshot of the UI widget tree for the session.

    ``root_id`` — 0 to start from all top-level windows; >0 to start from a
    specific element from a previous snapshot.
    ``max_depth`` — how many levels of child elements to include.
    0 = root element(s) only, 1 = root + direct children, etc.
    Use -1 for unlimited depth (entire tree).
    ``prop_depth`` — how many levels of QObject property values to expand.
    0 = no properties, 1 = direct properties only, etc.
    Use -1 for unlimited depth.
    """
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.snapshot", {
        "include_hidden": include_hidden,
        "detail": detail,
        "rootId": root_id,
        "maxDepth": max_depth,
        "propDepth": prop_depth,
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
    """Find UI elements matching a query in a session.

    Supported query fields: type, type_inherits, text, text_contains,
    object_name, window_title, window_title_contains, properties,
    ancestor_id, window_id.  The element map is refreshed (like a
    snapshot) before matching, so returned ids are always usable.
    """
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
    ss_dir = session.session_dir / "screenshots"
    ss_dir.mkdir(parents=True, exist_ok=True)

    # Sequence based on existing files so screenshots stay unique without
    # depending on the snapshot counter.
    seq = len(list(ss_dir.glob("screenshot_*.png"))) + 1

    # The injected library writes the PNG into `dir` itself and returns the
    # file path; pass the session's screenshot dir so it lands here.
    params = {"dir": str(ss_dir), "seq": seq}
    if element_id > 0:
        params["element_id"] = element_id
    result = await session.send_rpc("qt.screenshot", params)

    filename = f"screenshot_{seq:08d}.png"
    ss_path = ss_dir / filename

    if result.get("ok"):
        src = Path(result.get("path", ""))
        try:
            png_bytes = src.read_bytes()
        except OSError as exc:
            return json.dumps({"error": f"failed to read screenshot file {src}: {exc}"})
        ss_path.write_bytes(png_bytes)
        # The library names its file {seq:06d}_{uuid}.png; drop it to keep
        # the session dir tidy (we keep the canonical name above).
        try:
            src.unlink()
        except OSError:
            pass
    else:
        # Keep the failure payload readable through the resource endpoint.
        ss_path.write_text(json.dumps(result))

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
    result = await session.send_rpc("qt.click", {
        "element_id": element_id, "button": button, "modifiers": modifiers or [],
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_click_at(session_id: str, x: float, y: float, button: str = "left", modifiers: list[str] | None = None, window_id: int = 0) -> str:
    """Click at an exact window coordinate, exactly like a real mouse click at that position.

    x/y are relative to the target window's client area (same space as
    screenshots). The click goes through the real Qt input pipeline, so the
    hit test (scene graph for QML, widget tree for QtWidgets) determines
    what receives it -- identical behavior to a human clicking there.
    window_id: element id of a top-level window from a snapshot; 0 uses the
    session's first visible top-level window."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.clickAt", {
        "x": x, "y": y, "button": button,
        "modifiers": modifiers or [], "window_id": window_id,
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_click_region(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None) -> str:
    """Click at the center of an element's on-screen region.

    Unlike qt_mouse_click (which delivers straight to the element), this
    routes through the real Qt input pipeline with real hit testing: for a
    QML container (e.g. a Rectangle with a MouseArea inside) the scene graph
    hit test delivers the click to the MouseArea, exactly as a human click
    would."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.clickRegion", {
        "element_id": element_id, "button": button, "modifiers": modifiers or [],
    })
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_press(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None, x: float | None = None, y: float | None = None) -> str:
    """Press a mouse button on an element WITHOUT releasing it.

    Pair with qt_mouse_release to split a click, or qt_mouse_move for a
    drag (press -> move -> release).  x/y are optional element-local
    coordinates (element center when omitted)."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "button": button,
        "modifiers": modifiers or [],
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.mousePress", params)
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_release(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None, x: float | None = None, y: float | None = None) -> str:
    """Release a previously pressed mouse button on an element.

    Completes a press/release pair (a click) or finishes a drag started
    with qt_mouse_press + qt_mouse_move.  x/y are optional element-local
    coordinates (element center when omitted)."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "button": button,
        "modifiers": modifiers or [],
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.mouseRelease", params)
    return json.dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_move(session_id: str, element_id: int, x: float, y: float) -> str:
    """Move the mouse pointer to an element-local position (no buttons).

    Use between qt_mouse_press and qt_mouse_release to drag, or alone to
    hover.  x/y are coordinates relative to the element's top-left corner."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.mouseMove", {
        "element_id": element_id, "x": float(x), "y": float(y),
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
async def qt_key_combo(session_id: str, element_id: int, keys: str) -> str:
    """Send a keyboard shortcut to an element, e.g. "Ctrl+C", "Ctrl+Shift+A".

    Format: modifier names ("Ctrl", "Alt", "Shift", "Meta") joined with '+'
    followed by the key name ("C", "F5", "Enter", "Tab", "Escape", "Home",
    arrows, ...).  element_id 0 targets the widget that currently has focus.
    Delivered as a real key press/release pair with the modifier state set."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.keyCombo", {
        "element_id": element_id, "keys": keys,
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
