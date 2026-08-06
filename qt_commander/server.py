"""qt-commander MCP server — entry point."""
import json
import os
from pathlib import Path


def _dumps(obj, **kwargs):
    """Serialize with UTF-8 text (no \\uXXXX escapes).

    Qt UI text is overwhelmingly UTF-8; escaping every non-ASCII code
    point makes snapshots and property reads unreadable.
    """
    return json.dumps(obj, ensure_ascii=False, **kwargs)

from fastmcp import FastMCP

from .builder import BUILD_DIR, check_build_state, run_build, BuildState
from .environment_detector import (
    detect_vs_environments,
    detect_qt_environments,
    detect_mingw_toolchains,
)
from .errors import BuildRequiredError, SessionNotFoundError, tool_error
from .occlusion import prune_snapshot
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
    return _dumps({"processes": procs}, indent=2)


@mcp.tool()
async def qt_attach(pid: int) -> str:
    """Inject the helper library into a running Qt process and open a session.

    Find the target pid with qt_list_processes first (only Qt processes are
    listed, each with its qt_version/arch/bitness).  The build artifacts in
    .qt-commander/bin must match the target process: same Qt major
    (qt_major used in qt_build), same toolchain (msvc/mingw), same
    Debug/Release build type, same 64/32-bit — a mismatch makes injection
    fail with code 2002.  A process that is already attached fails with
    2006; detach first.

    If the build has not been completed, this returns error code 2001 with
    the steps to follow (qt_detect_msvc_and_qt → AskUserQuestion →
    qt_build → qt_attach again); it does not build automatically.

    Injection modifies the target process (loads a DLL, starts a local
    RPC thread/listener).  If the target process exits, the session goes
    dead: operations then fail with connection/timeout errors — check
    qt_list_sessions and re-attach.
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

    return _dumps({
        "session_id": session.id,
        "pid": pid,
        "connected": session.connected,
    })


@mcp.tool()
async def qt_detach(session_id: str, purge: bool = False) -> str:
    """Disconnect from a Qt process session.

    ``purge=True`` (default False) also **ejects the injected DLL from the
    target process** and **deletes the session's saved files** (snapshots,
    screenshots, session metadata) — irreversible.  Use purge=True before
    rebuilding the library: otherwise the DLL file stays locked by the
    target process and qt_build fails.  With purge=False the DLL stays
    loaded in the target until that process exits."""
    ok = await sessions.destroy(session_id, purge=purge)
    if not ok:
        return tool_error(-32602, f"Session not found: {session_id}")
    return _dumps({"status": "detached", "session_id": session_id, "purged": purge})


@mcp.tool()
async def qt_list_sessions() -> str:
    """List all active sessions."""
    return _dumps({"sessions": sessions.list_sessions()}, indent=2)


# ============================================================================
# Build Tools
# ============================================================================

@mcp.tool()
async def qt_detect_msvc_and_qt() -> str:
    """Detect MSVC (Visual Studio), MinGW toolchains, and Qt installations
    available for building.

    IMPORTANT — this tool only *discovers* and *displays* what is found on
    this machine.  It does NOT select an environment automatically.

    After calling this tool, the AI MUST use ``AskUserQuestion`` to present
    the found installations to the user and let them choose which compiler
    and Qt to use with ``qt_build``.  For an MSVC Qt kit
    (``kit == "msvc"``) pass the VS ``vcvars_path`` + the kit's
    ``qtenv_path`` with ``toolchain="msvc"``; for a MinGW kit
    (``kit == "mingw"``) pass a MinGW toolchain's bin dir (from
    ``mingw_toolchains``) + the kit's ``qtenv_path`` (MinGW kits ship
    qtenv2.bat too) with ``toolchain="mingw"``.
    If detection finds nothing (or misses an installation the user knows
    about), the user can type the paths manually.

    Returns three lists: ``vs_installations``, ``mingw_toolchains``, and
    ``qt_installations``.
    """
    vs_list = detect_vs_environments()
    mingw_list = detect_mingw_toolchains()
    qt_list = detect_qt_environments()
    return _dumps({
        "vs_installations": vs_list,
        "mingw_toolchains": mingw_list,
        "qt_installations": qt_list,
    }, indent=2)


@mcp.tool()
async def qt_build(
    vcvars_path: str = "",
    qt_env: str = "",
    vcvars_args: str = "",
    build_type: str = "Release",
    qt_major: int = 5,
    generator: str = "",
    with_qml: bool = True,
    toolchain: str = "msvc",
) -> str:
    """Build the Qt injection library and injector for the specified environment.

    ``toolchain`` selects the compiler: ``"msvc"`` (default; ``vcvars_path``
    is a vcvars bat, ``qt_env`` a qtenv2.bat) or ``"mingw"``
    (``vcvars_path`` is the MinGW bin dir; ``qt_env`` the kit's qtenv2.bat
    — MinGW Qt kits ship one too; the Qt-bundled Ninja is used
    automatically).

    Before calling this tool, the AI MUST present available build
    environments (from ``qt_detect_msvc_and_qt``) to the user and let them
    choose.  Do NOT guess or auto-pick paths — the user decides which VS
    or MinGW toolchain and which Qt installation to use.

    The AI MUST also ask the user whether to enable QML/QQuick support
    (``with_qml``).  QML support is required for inspecting QML/Qt Quick
    applications but adds build dependencies.

    The AI MUST also ask the user to choose the ``build_type`` (Debug or
    Release).  The build type must match the target process — a Debug
    process requires a Debug build, a Release process requires Release.

    ``qt_major`` (default 5) must match the target application's Qt major
    version — read it from qt_list_processes' ``qt_version`` field or from
    the qt_detect_msvc_and_qt result; a Qt6 target built with qt_major=5
    fails to inject.  ``vcvars_args`` — extra args for the VS vcvars batch
    (e.g. "-arch x64").  ``generator`` — CMake generator override; omit to
    auto-select.  Before rebuilding, detach any session that still holds
    the old DLL (qt_detach with purge=True) or the build fails on a locked
    file.
    """
    result = await run_build(
        vcvars_path=vcvars_path,
        qt_env=qt_env,
        vcvars_args=vcvars_args,
        build_type=build_type,
        qt_major=qt_major,
        generator=generator,
        with_qml=with_qml,
        toolchain=toolchain,
    )
    return _dumps(result, indent=2)


# ============================================================================
# UI Inspection Tools
# ============================================================================

@mcp.tool()
async def qt_snapshot(session_id: str, include_hidden: bool = False,
                       detail: str = "extended",
                       root_id: int = 0, max_depth: int = 1,
                       prop_depth: int = 1) -> str:
    """Capture the UI element tree of the session's windows.

    The tree is NOT in this result — it is written to the file exposed at
    the returned ``uri`` (resource `qt-commander://sessions/.../snapshots/
    snapshot_N.json`); read that resource to inspect the tree.  The result
    carries only session_id / snapshot_id / uri.  Node fields: className,
    objID, objectName, rect (window-local logical px), global_rect (screen
    coords), z_order, visible/enabled/opacity/color_alpha/clip, text,
    topLevelId, windowTitle, properties (per ``detail``).

    ``detail`` — property tier per node: "core" (first-class fields only,
    no properties), "extended" (DEFAULT; common interaction-state
    properties like text/checked/value), "full" (every Q_PROPERTY; slow on
    large UIs).
    ``include_hidden`` (default False) — hidden elements are pruned
    together with their whole subtree (hidden tabs, unopened dialogs).
    Set True to include them; note hidden elements are still rejected by
    click/input/property tools.
    ``max_depth`` — levels of children: 0 = roots only, 1 = roots +
    direct children, -1 = entire tree (default 1).
    ``prop_depth`` — levels of QObject property expansion (0 = none,
    -1 = unlimited).
    ``root_id`` — 0 = all top-level windows; >0 = subtree of an element
    from the MOST RECENT snapshot/find.  Ids expire on every refresh: if
    root_id no longer resolves, the tool silently falls back to all
    top-level windows — pass it only when you know it is fresh.

    ID LIFECYCLE: this call rebuilds the element map, so every element_id
    and window_id from previous snapshots/finds becomes invalid.  The
    snapshot runs on the target process's GUI thread (brief UI freeze);
    if the target is busy, calls can time out after ~30s.
    """
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.snapshot", {
        "include_hidden": include_hidden,
        "detail": detail,
        "rootId": root_id,
        "maxDepth": max_depth,
        "propDepth": prop_depth,
    })
    if result.get("ok") is False:
        # e.g. a stale root_id: surface the error directly, do not write a
        # misleading empty snapshot file.
        return _dumps(result, indent=2)
    session.snapshot_count += 1

    filename = f"snapshot_{session.snapshot_count:08d}.json"
    snap_path = session.session_dir / "snapshots" / filename
    # _dumps() emits UTF-8 text; write_text defaults to the locale encoding
    # (e.g. cp936 on a Chinese Windows), which would corrupt the file.
    snap_path.write_text(_dumps(result, indent=2), encoding="utf-8")

    return _dumps({
        "session_id": session_id,
        "snapshot_id": session.snapshot_count,
        "uri": f"qt-commander://sessions/{session_id}/snapshots/{filename}",
    })


@mcp.tool()
async def qt_prune_snapshot(session_id: str, snapshot_id: int) -> str:
    """Occlusion-prune a saved snapshot.

    ``snapshot_id`` is the value returned by the last qt_snapshot call of
    this session.  When to use: after a snapshot, when elements overlap
    inside the same window and you need to know which one is really on
    top / how much of a target is visible — e.g. before deciding which
    element to click or whether a covered element matters.

    Reads the saved snapshot JSON and computes what is actually visible:
    elements fully covered by higher-z opaque elements are removed (their
    still-visible descendants are reparented up), partially covered ones
    get a ``visible_ratio`` field, fully hidden (opacity 0) elements are
    dropped.  Writes ``snapshot_<id>_pruned.json`` next to the original —
    read the tree at the returned ``uri`` (same objIDs as the source
    snapshot plus ``visible_ratio`` annotations).  Result field
    ``pruned`` = {"removed", "kept", "removed_ratio"}; window roots are
    never removed.

    The solver is a geometric heuristic (axis-aligned rects, per-window
    z-order with same-z tree order; widgets and QML rectangles/images
    occlude unless semi-transparent, transparent containers, text and
    custom QML components do not; a parent never occludes its own
    children).  Occlusion is per top-level window on purpose: the agent
    must stay able to operate an app that the user has covered or
    minimised, so windows never occlude each other.  The objIDs in the
    pruned file expire with the next snapshot/find refresh like any other
    id.
    """
    session = _resolve_session(session_id)
    src = session.session_dir / "snapshots" / f"snapshot_{snapshot_id:08d}.json"
    if not src.exists():
        return _dumps({"error": f"snapshot {snapshot_id} not found in session"})
    snapshot = json.loads(src.read_text(encoding="utf-8"))
    pruned = prune_snapshot(snapshot)
    dst = session.session_dir / "snapshots" / \
        f"snapshot_{snapshot_id:08d}_pruned.json"
    dst.write_text(_dumps(pruned, indent=2), encoding="utf-8")
    return _dumps({
        "session_id": session_id,
        "source": f"snapshot_{snapshot_id:08d}.json",
        "uri": f"qt-commander://sessions/{session_id}/snapshots/{dst.name}",
        "pruned": pruned["pruned"],
    })


@mcp.tool()
async def qt_find_element(session_id: str, query: dict) -> str:
    """Find UI elements matching a query in a session.

    Query fields (all AND-ed): ``type`` (exact C++ class name),
    ``type_inherits`` (superclass chain — C++ classes only; custom QML
    components are generated classes like ``X_QMLTYPE_8`` and do NOT match
    a QML type name), ``text`` / ``text_contains`` (display text, CJK
    supported), ``object_name``, ``window_title`` /
    ``window_title_contains``, ``properties`` (all key-value pairs must
    match), ``ancestor_id`` / ``window_id`` (scope limits), ``depth``
    ("exact" = direct children, "shallow" = 2 levels, integer = that many,
    "deep"/omitted = whole tree), ``limit`` (max matches).  ``include_hidden``
    is a field INSIDE the query dict (default false) — hidden elements
    are only matched when it is true.

    Example: {"text_contains": "OK", "type_inherits": "QPushButton",
    "depth": "shallow"} or {"object_name": "searchBox", "limit": 5}.

    Result: {"ok": true, "count": N, "elements": [{id, className,
    objectName, ...}]}; when nothing matches: {"ok": false, "message":
    "No matching element found"} — adjust the query or set
    include_hidden: true.  No prior snapshot is needed; this call rebuilds
    the element map itself.

    ID LIFECYCLE: the rebuild invalidates EVERY element_id/window_id from
    previous snapshots/finds.  Use the returned ids immediately — if an
    operation reports "Element not found: id=N", the id is stale or the
    element was destroyed: re-run qt_find_element (or a snapshot) and
    retry with the fresh id; never reuse old ids.
    """
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.findElement", {"query": query})
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_get_property(session_id: str, element_id: int, name: str) -> str:
    """Read a property from a UI element.

    Read-only, no side effects on the target.  Note: hidden, disabled and
    zero-size elements are rejected even for reads ("Element is not
    visible") — pass an id from a recent find/snapshot of a visible
    element."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.getProperty", {
        "element_id": element_id, "name": name,
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_set_property(session_id: str, element_id: int, name: str, value: str) -> str:
    """Write a property value on a UI element.

    DESTRUCTIVE: directly modifies the target application's state (may
    trigger property-notify signals, change geometry/visibility/business
    state) with no undo.  Prefer real user input (clicks/typing) to
    simulate user actions; use this only when a direct API write is
    intended.

    ``value`` is first parsed as JSON: "true" -> bool, "123" -> int,
    "\\"hello\\"" -> string.  Unparseable text is sent as a plain string —
    pass quoted strings explicitly when a string is intended."""
    session = _resolve_session(session_id)
    try:
        parsed_value = json.loads(value)
    except (json.JSONDecodeError, TypeError):
        parsed_value = value
    result = await session.send_rpc("qt.setProperty", {
        "element_id": element_id, "name": name, "value": parsed_value,
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_call_method(session_id: str, element_id: int, method: str, args: list | None = None) -> str:
    """Invoke a QMetaObject-invokable method on a UI element.

    DESTRUCTIVE: directly changes the target application's state with no
    undo.  Methods like ``close()``, ``deleteLater()`` (destroys the
    element and invalidates its id) or any app slot execute for real.
    Prefer simulating user input (clicks/keys); call methods only when a
    direct API invocation is intended.  Argument types are coerced
    silently (up to 10 args)."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.callMethod", {
        "element_id": element_id, "method": method, "args": args or [],
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_screenshot(session_id: str, element_id: int = 0) -> str:
    """Capture a screenshot of a UI element or the entire window.

    ``element_id`` 0 captures the active (or first visible) top-level
    window.  The PNG is written to the file at the returned ``uri`` —
    read that resource (qt-commander://sessions/.../screenshots/N.png) to
    obtain the image bytes.  On failure the result reports the error and
    no image is written."""
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
            return _dumps({"error": f"failed to read screenshot file {src}: {exc}"})
        ss_path.write_bytes(png_bytes)
        # The library names its file {seq:06d}_{uuid}.png; drop it to keep
        # the session dir tidy (we keep the canonical name above).
        try:
            src.unlink()
        except OSError:
            pass
    if not result.get("ok"):
        # Report the failure instead of writing error text into a .png.
        return _dumps({"error": "screenshot failed", "detail": result},
                      indent=2)

    return _dumps({
        "session_id": session_id,
        "uri": f"qt-commander://sessions/{session_id}/screenshots/{filename}",
    })


# ============================================================================
# Interaction Tools
# ============================================================================

@mcp.tool()
async def qt_mouse_click(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None) -> str:
    """Send a mouse click to a UI element (direct delivery).

    For plain QtWidgets (QPushButton, QLineEdit, ...) direct delivery
    works and is the cheapest option.  Use qt_mouse_click_region (element
    center) or qt_mouse_click_at (exact coordinates) for QML custom
    components (buttons, nav rows, list items) — those route through the
    real Qt input pipeline with real hit testing and are far more
    reliable.  ``button``: "left"/"right"/"middle" (anything else falls
    back to left); ``modifiers``: list of "Ctrl"/"Alt"/"Shift"/"Meta".
    A result with ok:false means the element was rejected (hidden/
    disabled/zero-size/stale id) — re-find and retry."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.click", {
        "element_id": element_id, "button": button, "modifiers": modifiers or [],
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_dbl_click(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None, x: float | None = None, y: float | None = None) -> str:
    """Send a double-click to a UI element (direct delivery).

    x/y are optional logical-pixel coordinates relative to the element's
    top-left corner; either both or neither (omitted = element center)."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "button": button,
        "modifiers": modifiers or [],
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.dblClick", params)
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_click_at(session_id: str, x: float, y: float, button: str = "left", modifiers: list[str] | None = None, window_id: int = 0) -> str:
    """Click at an exact window coordinate, exactly like a real mouse click at that position.

    x/y are relative to the target window's client area (same space as
    screenshots, logical pixels — DPI is handled internally). The click
    goes through the real Qt input pipeline, so the hit test (scene graph
    for QML, widget tree for QtWidgets) determines what receives it —
    identical behavior to a human clicking there, including real
    consequences (menus, close buttons, destructive actions).  window_id:
    id of a top-level window from the MOST RECENT snapshot/find (ids
    expire on refresh); 0 uses the session's first visible top-level
    window — pass it explicitly when the session has multiple windows."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.clickAt", {
        "x": x, "y": y, "button": button,
        "modifiers": modifiers or [], "window_id": window_id,
    })
    return _dumps(result, indent=2)


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
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_press(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None, x: float | None = None, y: float | None = None) -> str:
    """Press a mouse button on an element WITHOUT releasing it.

    Pair with qt_mouse_release to split a click, or qt_mouse_move for a
    drag (press -> move -> release).  x/y are optional logical-pixel
    coordinates relative to the element's top-left corner; either both or
    neither (omitted = element center).  IMPORTANT: after a press you must
    release in the same session, or the target app stays in a pressed/
    dragging state."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "button": button,
        "modifiers": modifiers or [],
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.mousePress", params)
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_release(session_id: str, element_id: int, button: str = "left", modifiers: list[str] | None = None, x: float | None = None, y: float | None = None) -> str:
    """Release a previously pressed mouse button on an element.

    Completes a press/release pair (a click) or finishes a drag started
    with qt_mouse_press + qt_mouse_move.  x/y are optional logical-pixel
    coordinates relative to the element's top-left corner; either both or
    neither (omitted = element center)."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "button": button,
        "modifiers": modifiers or [],
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.mouseRelease", params)
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_wheel(session_id: str, element_id: int, dx: float = 0.0, dy: float = 0.0, x: float | None = None, y: float | None = None, pixel: bool = False) -> str:
    """Scroll the mouse wheel over an element.

    dx/dy are the wheel deltas in either pixel (``pixel=True``) or line
    (default) units.  x/y optionally position the wheel inside the
    element (element-local logical px, both or neither — omitted = center).
    Use this to scroll lists, tables, canvases and other scrollable
    content."""
    session = _resolve_session(session_id)
    params = {
        "element_id": element_id, "dx": dx, "dy": dy,
        "pixel": pixel,
    }
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.wheel", params)
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_move(session_id: str, element_id: int, x: float, y: float) -> str:
    """Move the mouse pointer to an element-local position (no buttons).

    Use between qt_mouse_press and qt_mouse_release to drag, or alone to
    hover.  x/y are coordinates relative to the element's top-left corner."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.mouseMove", {
        "element_id": element_id, "x": float(x), "y": float(y),
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_mouse_context_menu(session_id: str, element_id: int, x: float | None = None, y: float | None = None) -> str:
    """Open the context menu at an element (right-click menu).

    x/y are optional logical-pixel coordinates relative to the element's
    top-left corner; either both or neither (omitted = element center)."""
    session = _resolve_session(session_id)
    params = {"element_id": element_id}
    if x is not None and y is not None:
        params["x"] = float(x)
        params["y"] = float(y)
    result = await session.send_rpc("qt.contextMenu", params)
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_keyboard_input(session_id: str, element_id: int, text: str, modifiers: list[str] | None = None) -> str:
    """Send keyboard input (typed text) to a UI element.

    Real input: the text lands in whatever the target would receive — it
    can overwrite existing text and trigger real app behaviour.  If
    element_id is 0 or does not resolve, the input goes to the widget
    that currently has focus (be careful: a stale id silently redirects
    the text).  Widgets that rely on focus (QLineEdit etc.) may ignore
    input to an unfocused element — call qt_focus first when text does
    not land.  Use qt_key_combo for shortcuts like Enter or Ctrl+C."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.typeText", {
        "element_id": element_id, "text": text, "modifiers": modifiers or [],
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_key_combo(session_id: str, element_id: int, keys: str) -> str:
    """Send a keyboard shortcut to an element, e.g. "Ctrl+C", "Ctrl+Shift+A".

    Format: modifier names ("Ctrl", "Alt", "Shift", "Meta") joined with '+'
    followed by the key name ("C", "F5", "Enter", "Tab", "Escape", "Home",
    arrows, ...).  element_id 0 targets the widget that currently has focus.
    Delivered as a real key press/release pair with the modifier state set —
    shortcuts can trigger real app actions (close window, save, submit)."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.keyCombo", {
        "element_id": element_id, "keys": keys,
    })
    return _dumps(result, indent=2)


@mcp.tool()
async def qt_focus(session_id: str, element_id: int) -> str:
    """Set focus on a UI element.

    Best effort: QWidget focus works directly; some QML items may need an
    explicit focus request from within the app and report ok without
    changing focus."""
    session = _resolve_session(session_id)
    result = await session.send_rpc("qt.focus", {"element_id": element_id})
    return _dumps(result, indent=2)


# ============================================================================
# Resources
# ============================================================================

@mcp.resource("qt-commander://sessions/{session_id}/snapshots/{filename}")
async def read_snapshot_resource(session_id: str, filename: str) -> str:
    """Read a snapshot resource."""
    if ".." in filename or "/" in filename or "\\" in filename:
        return _dumps({"error": "invalid filename"})
    sess = _resolve_session(session_id)
    path = (sess.session_dir / "snapshots" / filename).resolve()
    if not str(path).startswith(str(sess.session_dir.resolve())):
        return _dumps({"error": "path traversal detected"})
    if not path.exists():
        return _dumps({"error": "resource not found"})
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
    """Entry point — run with: uv run python -m qt_commander."""
    mcp.run()
