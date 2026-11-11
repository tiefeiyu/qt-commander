"""qt-commander MCP server (linked mode vertical slice)."""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
import secrets
import shutil
import subprocess
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

# FastMCP asks PyPI for a newer version while printing its startup banner. That
# request goes through httpx, which hard-fails when a SOCKS proxy is configured
# without the socksio extra ("Using SOCKS proxy, but the 'socksio' package is
# not installed"), taking the whole server down before it serves anything. A
# local MCP server should not need network access to boot, so the check is off
# by default; set FASTMCP_CHECK_FOR_UPDATES=stable to turn it back on. This has
# to happen before fastmcp.settings is imported.
os.environ.setdefault("FASTMCP_CHECK_FOR_UPDATES", "off")

import psutil  # noqa: E402
from fastmcp import FastMCP  # noqa: E402
from fastmcp.exceptions import ValidationError as ToolValidationError  # noqa: E402
from fastmcp.server.middleware import Middleware  # noqa: E402
from fastmcp.tools import ToolResult  # noqa: E402
from fastmcp.utilities.types import Image  # noqa: E402

from . import artifacts, inject, launch as launcher, ops, target
from .client import ClientClosed, ClientTimeout, RpcClient, RpcError
from .environment import detect_qt_environments, detect_toolchains
from .errors import ErrorCode
from .session import Session, SessionExists, SessionManager, SessionNotFound

#: Resolved once at import: the tool set and the instructions must describe the
#: same platform capabilities.
LAUNCH_SUPPORTED = launcher.supported()

_BASE_INSTRUCTIONS = """\
qt-commander is for autonomous operation of a running Qt/QML application.

When to use:
- A real QWidget or QML/QQuickWindow UI must be inspected or driven: locate
  controls, click/type, read/write properties, call slots, capture a
  screenshot, or verify a UI state.
- Not for static code/log analysis, non-Qt targets, or process launch/exit.
  If no target is running, use the project's own launch procedure first.

How to use:
1. If the target already has a linked descriptor, use qt_status and
   qt_attach(descriptor=...). No build or injection is needed.
2. If the target PID is unknown, use qt_processes to discover Qt targets.
   Otherwise use qt_target_info (and qt_environments) to inspect the target
   architecture, Qt major version, toolchain and Qt debug/release. Then call
   qt_build(for_pid=<pid>) to build the matching injector; it derives the
   arguments, writes one artifact per target, and reports matches_target,
   restart_required and next_step.
3. qt_attach(pid=..., mode="injection"), then qt_windows to choose a window.
   Keep one session per process.
Step 4: qt_query to locate a target; then act/read/write/call.
5. Verify with query/get/screenshot, collect any pending task with qt_task,
   close popups you opened, and qt_detach.

Long-running calls:
- qt_act/qt_call/qt_set return {"state": "running", "task_id": ...} when the
  call is still executing in the target after wait_ms (1000 ms for act, 3000 ms
  for call/set). This is normal when the action opened a modal dialog.
- The action has already been dispatched: do not retry it. Call
  qt_task(task_id=<id>) to collect the result once the target finishes.
- The session is not blocked while a task runs: qt_windows/qt_query/qt_act and
  qt_task keep working on the same session, so drive the dialog instead of
  waiting.

Critical rules:
- The tree is live; re-query when a target may be stale. A successful act is
  not proof; verify the resulting state.
- A "running" task is not a failure and not a timeout; it means the target is
  still inside the call. Collect it with qt_task before acting on the result.
- Targets: returned id, string QML id, or {"qml_id":...}/{"text":...}/
  {"class":...}/{"object_name":...}. Combine fields for repeated delegates;
  use nth for ambiguity.
- E_AMBIGUOUS: inspect candidates. E_HIDDEN: hidden by design; use
  include_hidden=true only when intentional.
- QML Popup roots may not resolve by qml_id; query their text or child items.
  A Popup in the main window Overlay is not a separate qt_windows entry.
- qt_call only invokes slots/Q_INVOKABLE; property setters use qt_set. Use
  qt_methods and signature= for overloads.
- Injection: qt_attach failures carry error.reason and error.details
  (target snapshot, chosen artifact, loaded injector, expected artifact).
  If error.restart_required is true the target process must be restarted;
  rebuilding the artifact does not replace a library already mapped into it.
- The injector build type must match the target's Qt runtime (Qt5Core.dll vs
  Qt5Cored.dll). qt_target_info.debug comes from the loaded runtime; never
  build against a kit directory that ships both.
- There is no build-id validation on attach; compare
  qt_target_info.loaded_injector.build_id with expected_artifact.build_id.
- On timeout or failure, inspect first. Retry at most once with a changed
  query/wait, then stop and report.
- Confirm before irreversible actions (delete/upload/exit/restart or
  persistent setting changes). Do not bypass hidden/safety checks.
"""

_LAUNCH_INSTRUCTIONS = """
Starting a target (Linux and macOS only):
- When the application is not running yet, qt_launch(program, args=[...]) starts
  it with the runtime preloaded (LD_PRELOAD / DYLD_INSERT_LIBRARIES), waits for
  it to register, and attaches. It accepts a macOS .app bundle.
- On macOS this is the only way in: the system refuses injection into an
  already-running hardened or notarized process. On Linux it avoids ptrace and
  needs no Yama configuration.
- qt_launch is absent on Windows, which has no preload mechanism; start the app
  yourself and use qt_attach(pid=..., mode="injection").
"""

INSTRUCTIONS = _BASE_INSTRUCTIONS + (
    _LAUNCH_INSTRUCTIONS if LAUNCH_SUPPORTED else ""
)

mcp = FastMCP("qt-commander", instructions=INSTRUCTIONS)
sessions = SessionManager()


def _rendezvous_dir() -> Path:
    return Path(os.environ.get(
        "QT_COMMANDER_RENDEZVOUS",
        str(Path(tempfile.gettempdir()) / "qt-commander"),
    ))


def _descriptor_candidates() -> list[Path]:
    roots = [_rendezvous_dir(), Path(tempfile.gettempdir()) / "qt-commander"]
    candidates: list[Path] = []
    seen: set[Path] = set()
    for root in roots:
        for path in sorted(root.glob("descriptor-*.json")):
            resolved = path.resolve()
            if resolved not in seen:
                seen.add(resolved)
                candidates.append(path)
        legacy = root / "descriptor.json"
        if legacy.exists() and legacy.resolve() not in seen:
            seen.add(legacy.resolve())
            candidates.append(legacy)
    return candidates


def _default_descriptor() -> Path | None:
    candidates = _descriptor_candidates()
    return candidates[0] if len(candidates) == 1 else None


def _descriptor_pid(path: Path) -> int | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return int(data["pid"]) if isinstance(data, dict) else None
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        return None


def _descriptor_session_id(path: Path) -> str | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        value = data.get("session_id") if isinstance(data, dict) else None
        return value if isinstance(value, str) else None
    except (OSError, json.JSONDecodeError):
        return None


def _live_descriptors(pid: int | None = None) -> list[Path]:
    result: list[Path] = []
    for path in _descriptor_candidates():
        if pid is not None and _descriptor_pid(path) != pid:
            continue
        if _descriptor_summary(path).get("pid_alive") is True:
            result.append(path)
    return result


def _descriptor_summary(path: Path) -> dict[str, Any]:
    summary: dict[str, Any] = {"path": str(path)}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        summary["error"] = str(exc)
        return summary
    if not isinstance(data, dict):
        summary["error"] = "descriptor root must be an object"
        return summary
    pid = data.get("pid")
    summary.update({
        "pid": pid if isinstance(pid, int) else None,
        "session_id": data.get("session_id"),
        "started_at": data.get("started_at"),
        "build_id": data.get("library_build_id") or data.get("library_version"),
        "process_start_time": data.get("process_start_time"),
    })
    if isinstance(pid, int):
        summary["pid_alive"] = psutil.pid_exists(pid)
    return summary


def _gc_descriptors(min_age_s: float = 300.0) -> int:
    """Delete descriptors whose process is gone (best effort, never raises).

    Only called from write paths (attach/detach): ``qt_status`` stays read-only.
    """
    removed = 0
    now = time.time()
    for path in _descriptor_candidates():
        try:
            if path.is_symlink() or not path.is_file():
                continue
            summary = _descriptor_summary(path)
            pid = summary.get("pid")
            if not isinstance(pid, int) or summary.get("pid_alive") is not False:
                continue
            if now - path.stat().st_mtime < min_age_s:
                continue
            if psutil.pid_exists(pid):
                continue
            path.unlink()
            removed += 1
        except OSError:
            continue
    return removed


def _error(code: str, message: str, *, retryable: bool = False,
           hint: str | None = None) -> dict[str, Any]:
    error: dict[str, Any] = {
        "code": code,
        "message": message,
        "retryable": retryable,
    }
    if hint:
        error["hint"] = hint
    return {"ok": False, "error": error}


def _map_error(exc: BaseException) -> dict[str, Any]:
    if isinstance(exc, RpcError):
        result = _error(exc.code, exc.message, retryable=exc.retryable,
                        hint=exc.hint)
        if exc.details:
            result["error"]["details"] = exc.details
        return result
    if isinstance(exc, ClientTimeout):
        return _error(
            ErrorCode.TARGET_TIMEOUT.value,
            str(exc),
            retryable=False,
            hint="request deadline exceeded; execution state is unknown",
        )
    if isinstance(exc, ClientClosed):
        return _error(ErrorCode.LINKED_UNREACHABLE.value, str(exc))
    if isinstance(exc, (ValueError, TypeError)):
        return _error(ErrorCode.INVALID_ARGUMENT.value, str(exc))
    return _error(ErrorCode.PROTOCOL.value, str(exc))


def _session(session_id: str) -> Session | dict[str, Any]:
    try:
        return sessions.get(session_id)
    except SessionNotFound:
        return _error(
            ErrorCode.SESSION_EXPIRED.value,
            f"unknown session: {session_id}",
        )


def _read_inject_status(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


async def _await_descriptor_for_pid(
    pid: int, timeout_s: float = 20.0, status_path: Path | None = None
) -> Path | None:
    """Wait for a descriptor; stop early when the library reports a failure.

    The injected library gives up after ~10 s and writes its reason to
    ``qtc-inject-status-<pid>.txt``. Polling that file shortens the failure
    from the full 20 s timeout to roughly the library's own budget, and the
    reason is available immediately.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            candidates = [p for p in _descriptor_candidates()
                          if _descriptor_pid(p) == pid]
        except OSError:
            candidates = []
        if candidates:
            try:
                return max(candidates, key=lambda p: p.stat().st_mtime)
            except OSError:
                pass
        if status_path is not None:
            message = _read_inject_status(status_path)
            if message and message != "ok":
                return None
        await asyncio.sleep(0.1)
    return None


def _target_brief(info: dict[str, Any] | None) -> dict[str, Any] | None:
    if not isinstance(info, dict) or not info:
        return None
    keys = ("pid", "os", "arch", "qt_major", "qt_version", "debug",
            "toolchain", "compiler_version", "detected_from")
    return {key: info[key] for key in keys if info.get(key) is not None}


def _loaded_brief(loaded: dict[str, Any] | None) -> dict[str, Any]:
    loaded = loaded or {}
    keys = ("loaded", "path", "artifact_dir", "build_id", "stale",
            "matches_expected", "serving", "artifact")
    return {key: loaded.get(key) for key in keys if loaded.get(key) is not None}


def _artifact_brief(library: Path) -> dict[str, Any]:
    directory = library.parent
    if directory.name.lower() in {"lib", "bin"}:
        directory = directory.parent
    manifest = artifacts.read_manifest(directory) or {}
    qt = manifest.get("qt") or {}
    return {
        "directory": str(directory),
        "library": str(library),
        "build_id": artifacts.manifest_build_id(manifest),
        "build_type": manifest.get("build_type"),
        "qt_debug": qt.get("debug"),
    }


def _attach_failure(
    code: str,
    reason: str,
    message: str,
    *,
    target_info: dict[str, Any] | None = None,
    artifact: dict[str, Any] | None = None,
    loaded: dict[str, Any] | None = None,
    expected: str | None = None,
    restart_required: bool = False,
    hint: str | None = None,
    retryable: bool = False,
) -> dict[str, Any]:
    """Build an attach error an agent can act on without reading the source."""
    result = _error(code, message, retryable=retryable, hint=hint)
    error = result["error"]
    error["reason"] = reason
    if restart_required:
        error["restart_required"] = True
    brief_target = _target_brief(target_info)
    if brief_target:
        error.setdefault("details", {})["target"] = brief_target
    if artifact is not None:
        error.setdefault("details", {})["artifact"] = artifact
    if loaded is not None:
        error.setdefault("details", {})["loaded_injector"] = _loaded_brief(loaded)
    if expected is not None:
        error.setdefault("details", {})["expected_artifact"] = expected
    return result


def _diagnose_injection_failure(
    message: str,
    info: dict[str, Any] | None,
    artifact: dict[str, Any],
    expected: str | None,
) -> tuple[str, str]:
    """Map a library-written status message to a reason and a next step."""
    text = (message or "").lower()
    info = info or {}
    target_debug = info.get("debug")
    artifact_debug = artifact.get("qt_debug")
    pid = info.get("pid")
    mismatch = (
        target_debug is not None
        and artifact_debug is not None
        and bool(artifact_debug) != bool(target_debug)
    )
    if "timeout waiting for qcoreapplication" in text:
        if mismatch:
            linked = "Qt5Cored.dll" if artifact_debug else "Qt5Core.dll"
            loaded_flavor = "debug" if target_debug else "release"
            hint = (
                f"the artifact is {'Debug' if artifact_debug else 'Release'} "
                f"({linked}) but the target loaded a {loaded_flavor} Qt "
                "runtime; the injected library can never see that "
                "QCoreApplication instance. Restart the target, rebuild with "
                f"qt_build(for_pid={pid}), then attach again."
            )
            if expected and artifact.get("directory") != expected:
                hint += f" The matching artifact is {expected}."
            return "qt_runtime_flavor_mismatch", hint
        return (
            "timeout_waiting_qcoreapp",
            "the injected library waited 10 s for a QCoreApplication and gave "
            "up; confirm the target really is a Qt application, then restart "
            "the target if a previous injector attempt may still be loaded",
        )
    return (
        "injection_failed",
        "the injected library reported a failure; read details.artifact and "
        "retry after restarting the target",
    )


async def _inject_target(pid: int,
                         artifact_dir: str | None = None) -> Path | dict[str, Any]:
    info: dict[str, Any] = {}
    info_error: str | None = None
    try:
        info = await asyncio.to_thread(target.target_info, pid)
    except (ValueError, OSError) as exc:
        info = {}
        info_error = str(exc)
    if not info:
        return _attach_failure(
            ErrorCode.PROCESS_GONE.value,
            "target_gone",
            info_error or f"cannot inspect pid {pid}",
            hint="verify the pid with qt_processes; the process may have exited",
        )
    expected = (info.get("expected_artifact") or {}).get("directory")
    loaded = info.get("loaded_injector")
    if not isinstance(loaded, dict):
        loaded = await asyncio.to_thread(
            target.loaded_injector, pid, expected
        )
    if loaded.get("loaded") and not _live_descriptors(pid):
        return _attach_failure(
            ErrorCode.INJECTION_FAILED.value,
            "injector_already_loaded",
            f"target already has an injector library loaded: "
            f"{loaded.get('path')}",
            target_info=info,
            loaded=loaded,
            expected=expected,
            restart_required=True,
            hint=(
                "restart the target process, then qt_attach(pid=..., "
                "mode=\"injection\") again. Loading the same library path a "
                "second time only increments its reference count, so DllMain "
                "does not run and no new runtime can start."
            ),
        )
    dll = inject.default_dll_path(pid, artifact_dir)
    if dll is None:
        detail = inject.artifact_detail(pid, artifact_dir)
        ambiguous = detail.get("error") == ErrorCode.ARTIFACT_AMBIGUOUS.value
        result = _attach_failure(
            ErrorCode.INJECTION_FAILED.value,
            "artifact_ambiguous" if ambiguous else "artifact_missing",
            ("multiple injector artifacts match the target" if ambiguous
             else "no injector artifact matches the target"),
            target_info=info,
            loaded=loaded,
            expected=expected,
            hint=(
                "retry qt_attach(pid=..., artifact_dir=<candidate>) or remove "
                "the stale duplicate artifact directory"
                if ambiguous else
                "run qt_build(for_pid=...) to build the matching injector, or "
                "set QT_COMMANDER_INJECT_DLL for an explicit override"
            ),
        )
        details = result["error"].setdefault("details", {})
        details["artifact_lookup"] = detail
        return result
    injector = inject.default_injector_path(pid, artifact_dir)
    status_path = Path(tempfile.gettempdir()) / f"qtc-inject-status-{pid}.txt"
    try:
        status_path.unlink(missing_ok=True)
    except OSError:
        pass
    try:
        await asyncio.to_thread(inject.inject_library, pid, dll,
                                artifact_dir=artifact_dir,
                                injector_path=injector)
    except inject.InjectionError as exc:
        reason = ("permission_denied"
                  if exc.code == ErrorCode.PERMISSION.value
                  else "injection_failed")
        return _attach_failure(
            exc.code, reason, str(exc),
            target_info=info,
            artifact=_artifact_brief(dll),
            loaded=loaded,
            expected=expected,
            hint=(
                "qtc-inject could not load the library; check process "
                "permissions (elevation, ptrace scope) and that the artifact "
                "matches the target architecture"
            ),
        )
    path = await _await_descriptor_for_pid(pid, status_path=status_path)
    if path is not None:
        return path
    message = _read_inject_status(status_path)
    artifact = _artifact_brief(dll)
    if message and message != "ok":
        reason, hint = _diagnose_injection_failure(
            message, info, artifact, expected
        )
        return _attach_failure(
            ErrorCode.INJECTION_FAILED.value, reason, message,
            target_info=info,
            artifact=artifact,
            loaded=loaded,
            expected=expected,
            restart_required=True,
            hint=hint,
        )
    return _attach_failure(
        ErrorCode.TARGET_NOT_QT.value,
        "target_not_ready",
        "DLL loaded but no descriptor appeared; target may not be a Qt app "
        "or its event loop may not have started yet",
        target_info=info,
        artifact=artifact,
        loaded=loaded,
        expected=expected,
        hint=(
            "wait for the target to finish starting, retry qt_attach, or pass "
            "artifact_dir if the selected artifact looks wrong"
        ),
        retryable=True,
    )


@mcp.tool
async def qt_status(session_id: str | None = None) -> dict[str, Any]:
    """Return live sessions/descriptors, optionally probing one session.

    Pass ``session_id`` to probe an existing session; otherwise this lists
    descriptor files and their process liveness.
    """
    candidates = _descriptor_candidates()
    summaries = [_descriptor_summary(path) for path in candidates]
    alive = [item for item in summaries if item.get("pid_alive") is True]
    dead = [item for item in summaries if item.get("pid_alive") is not True]
    recent_dead = sorted(
        dead, key=lambda item: str(item.get("started_at") or ""), reverse=True
    )[:5]
    live = _live_descriptors()
    result: dict[str, Any] = {
        "sessions": [asdict(info) for info in sessions.list()],
        "descriptors": [str(path) for path in candidates],
        "descriptor_info": alive + recent_dead,
        "descriptor_total": len(summaries),
        "descriptor_dead": len(dead),
        "descriptor": str(live[0]) if len(live) == 1 else None,
        "artifacts": artifacts.listing(),
        "protocol": 2,
    }
    if session_id is None:
        return result
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    result["session"] = {
        "session_id": session.session_id,
        "pid": session.pid,
        "connected": session.connected,
        "grace_remaining_ms": session.grace_remaining_ms,
    }
    try:
        result["ping"] = await session.client.ping(timeout=2.0)
        result["alive"] = True
    except (ClientTimeout, ClientClosed, RpcError, OSError) as exc:
        result["alive"] = False
        result["ping_error"] = _map_error(exc)["error"]
    return result


@mcp.tool
async def qt_target_info(
    pid: int | None = None,
    descriptor: str | None = None,
) -> dict[str, Any]:
    """Inspect a target process's architecture and Qt runtime before building.

    Use this before qt_build when injecting a running target. Reads the
    executable image, the modules actually loaded into the process, the loader
    PATH, Qt runtime DLLs, and descriptor build id; it does not inject or
    launch anything.

    ``qt_version`` is the full version (for example 5.15.2) when the loaded
    runtime or the descriptor provides it; ``build_type`` comes from the
    descriptor when available. ``detected_from`` says which evidence won:
    ``module`` (the Qt runtime mapped into the process, authoritative),
    ``adjacent``, ``loader_path`` or ``descriptor``.

    ``expected_artifact`` is the artifact ``qt_attach`` would inject and
    ``loaded_injector`` describes the injector library already mapped into the
    target (path, artifact, build_id, stale). ``injection_state`` is
    ``none``/``loaded``/``stale``/``mismatch``; ``restart_required`` means a
    loaded injector cannot be replaced without restarting the process.
    """
    if pid is None and descriptor is None:
        candidates = _live_descriptors()
        if len(candidates) != 1:
            return _error(
                "E_AMBIGUOUS",
                "provide pid or descriptor (or start exactly one linked app)",
            )
        descriptor = str(candidates[0])
    if descriptor is None and pid is not None:
        # Reuse the linked descriptor so build_type/Qt metadata can be read
        # from the runtime that actually created the process.
        for candidate in _descriptor_candidates():
            if _descriptor_pid(candidate) == pid:
                descriptor = str(candidate)
                break
    try:
        return target.target_info(pid=pid, descriptor=descriptor)
    except ValueError as exc:
        return _error("E_NOT_FOUND", str(exc))


@mcp.tool
async def qt_processes(
    name: str | None = None,
    limit: int = 50,
    offset: int = 0,
    include_non_qt: bool = False,
) -> dict[str, Any]:
    """List accessible running Qt targets without injecting.

    Discovery uses live descriptors or a Qt Core runtime next to the
    executable. Each entry includes pid, arch, Qt version (when known), and
    an ``attach`` hint. Use before ``qt_attach(pid=..., mode="injection")``.
    """
    if limit < 1 or limit > 200:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "limit must be between 1 and 200")
    if offset < 0:
        return _error(ErrorCode.INVALID_ARGUMENT.value, "offset must be >= 0")
    descriptors: dict[int, Path] = {}
    for candidate in _descriptor_candidates():
        pid = _descriptor_pid(candidate)
        if pid is not None:
            descriptors[pid] = candidate
    return await asyncio.to_thread(
        target.list_processes,
        descriptors,
        name,
        limit,
        offset,
        include_non_qt,
    )


@mcp.tool
async def qt_windows(
    session_id: str,
    include_hidden: bool = False,
    visible: bool | None = None,
    class_name: str | None = None,
    limit: int = 200,
    offset: int = 0,
) -> dict[str, Any]:
    """List top-level windows with id/title/class/screen_pos/active/modal.

    Use after attach to choose the window to inspect. ``include_hidden`` also
    returns hidden QWidget/QQuickWindow roots.
    ``visible`` and ``class_name`` filter the result; ``limit``/``offset``
    paginate. Window ids are accepted by qt_act and qt_screenshot; element ids
    from qt_query/qt_get are accepted by every target-taking tool.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.windows(
            session,
            include_hidden=include_hidden,
            visible=visible,
            class_name=class_name,
            limit=limit,
            offset=offset,
        )
    except Exception as exc:
        return _map_error(exc)


def _qt_commander_environment() -> dict[str, Any]:
    """Describe this install so a caller can build without reading the repo."""
    root = artifacts.default_root()
    source = Path(__file__).resolve().parents[1]
    source_dir = str(source) if (source / "CMakeLists.txt").is_file() else None
    available = []
    for entry in artifacts.listing():
        manifest = entry["manifest"]
        available.append({
            "directory": entry["directory"],
            "build_id": entry.get("build_id"),
            "version": entry.get("version"),
            "commit": entry.get("commit"),
            "build_type": manifest.get("build_type"),
            "qt": manifest.get("qt"),
            "compiler": manifest.get("compiler"),
        })
    return {
        "source_dir": source_dir,
        "artifact_root": str(root),
        "artifacts": available,
    }


@mcp.tool
async def qt_environments() -> dict[str, Any]:
    """Detect Qt kits, C++ toolchains and this qt-commander install.

    Read-only; use it before qt_build when the Qt path/toolchain is unknown.
    ``qt_commander`` reports the source directory ``qt_build`` defaults to, the
    artifact root, and every existing artifact with its ``build_id``. When
    exactly one live descriptor exists, ``suggested_build_call`` holds a ready
    ``qt_build`` invocation for that target.
    """
    def detect() -> dict[str, Any]:
        return {
            "qt": detect_qt_environments(),
            "toolchains": detect_toolchains(),
            "qt_commander": _qt_commander_environment(),
        }

    try:
        result = await asyncio.to_thread(detect)
    except Exception as exc:
        return _map_error(exc)
    live = _live_descriptors()
    if len(live) == 1:
        pid = _descriptor_pid(live[0])
        if pid is not None:
            result["suggested_build_call"] = {
                "tool": "qt_build",
                "arguments": {"for_pid": pid},
            }
    return result


def _mingw_compilers() -> tuple[str, str] | None:
    cxx = shutil.which("g++")
    cc = shutil.which("gcc")
    if cxx and cc:
        return cc, cxx
    tools = Path("C:/Software/Qt/Tools")
    if tools.is_dir():
        for candidate in sorted(tools.glob("mingw*")):
            candidate_cxx = candidate / "bin" / "g++.exe"
            candidate_cc = candidate / "bin" / "gcc.exe"
            if candidate_cxx.is_file() and candidate_cc.is_file():
                return str(candidate_cc), str(candidate_cxx)
    return None


def _msvc_command(vcvars: str, args: list[str], script: Path) -> list[str]:
    command = subprocess.list2cmdline(args)
    script.write_text(
        f'@echo off\r\ncall "{vcvars}" >nul\r\n{command}\r\n',
        encoding="utf-8",
    )
    return ["cmd.exe", "/d", "/c", str(script)]


async def _run_build_command(
    args: list[str], timeout_s: int
) -> tuple[int, str]:
    process = await asyncio.create_subprocess_exec(
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout_s)
    except asyncio.TimeoutError:
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/T", "/F", "/PID", str(process.pid)],
                capture_output=True,
                check=False,
            )
        else:
            process.kill()
        await process.wait()
        return -1, f"command timed out after {timeout_s}s"
    except asyncio.CancelledError:
        # Server shutdown / task cancellation must not leave an orphan ninja
        # writing into the build tree.
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/T", "/F", "/PID", str(process.pid)],
                capture_output=True,
                check=False,
            )
        else:
            process.kill()
        try:
            await process.wait()
        except Exception:
            pass
        raise
    return process.returncode or 0, output.decode("utf-8", "replace")


def _cmake_cache_home(cache: Path) -> str | None:
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                return line.split("=", 1)[1].strip() or None
    except OSError:
        return None
    return None


@dataclass
class _BuildPlan:
    configure_command: list[str]
    build_command: list[str]
    build: Path
    build_id: str
    artifact_dir: Path
    os_name: str
    arch: str
    toolchain: str
    compiler_version: str | None
    qt_version: str
    qt_debug: bool
    build_type: str
    timeout_s: int


@dataclass
class _BuildJob:
    job_id: str
    key: str
    plan: _BuildPlan
    status: str = "running"
    stage: str = "configure"
    started_at: float = field(default_factory=time.monotonic)
    finished_at: float | None = None
    output: str = ""
    result: dict[str, Any] | None = None
    task: asyncio.Task | None = None


_build_jobs: dict[str, _BuildJob] = {}
_build_jobs_by_key: dict[str, str] = {}


def _job_key(plan: _BuildPlan) -> str:
    return "|".join([
        str(plan.build),
        plan.build_id,
        plan.toolchain,
        plan.compiler_version or "",
        plan.build_type,
        str(plan.timeout_s),
    ])


def _job_summary(job: _BuildJob) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "job_id": job.job_id,
        "status": job.status,
        "stage": job.stage,
        "build_id": job.plan.build_id,
        "build_dir": str(job.plan.build),
        "artifact_dir": str(job.plan.artifact_dir),
        "elapsed_s": round(
            (job.finished_at or time.monotonic()) - job.started_at, 1
        ),
    }
    if job.output:
        summary["output_tail"] = job.output[-2000:]
    if job.result is not None:
        summary["result"] = job.result
    return summary


def _build_result(plan: _BuildPlan, *, ok: bool, stage: str,
                  message: str, details: dict[str, Any] | None = None,
                  output: str = "") -> dict[str, Any]:
    result: dict[str, Any] = {
        "ok": ok,
        "status": "ok" if ok else "failed",
        "build_id": plan.build_id,
        "build_dir": str(plan.build),
        "artifact_dir": str(plan.artifact_dir),
        "stage": stage,
        "compiler_version": plan.compiler_version,
    }
    if not ok:
        error: dict[str, Any] = {
            "code": ErrorCode.BUILD_FAILED.value,
            "message": message,
        }
        if details:
            error["details"] = details
        result["error"] = error
    if output:
        result["output"] = output[-4000:]
    return result


async def _run_build_job(job: _BuildJob) -> None:
    plan = job.plan
    deadline = time.monotonic() + plan.timeout_s
    try:
        job.stage = "configure"
        code, output = await _run_build_command(
            plan.configure_command,
            max(1, int(deadline - time.monotonic())),
        )
        job.output = output
        if code == -1:
            job.result = _build_result(
                plan, ok=False, stage="timeout",
                message="cmake configure timed out", output=output)
        elif code != 0:
            job.result = _build_result(
                plan, ok=False, stage="configure",
                message="cmake configure failed",
                details={"exit_code": code, "output": output[-8000:]},
                output=output)
        else:
            job.stage = "build"
            code, output = await _run_build_command(
                plan.build_command,
                max(1, int(deadline - time.monotonic())),
            )
            job.output = output
            if code == -1:
                job.result = _build_result(
                    plan, ok=False, stage="timeout",
                    message="cmake build timed out", output=output)
            elif code != 0:
                job.result = _build_result(
                    plan, ok=False, stage="build",
                    message="cmake build failed",
                    details={"exit_code": code, "output": output[-8000:]},
                    output=output)
            else:
                job.stage = "artifact"
                job.result = _collect_artifacts(plan, output)
    except asyncio.CancelledError:
        job.status = "cancelled"
        job.stage = "cancelled"
        job.finished_at = time.monotonic()
        raise
    except OSError as exc:
        job.result = _build_result(
            plan, ok=False, stage=job.stage, message=str(exc))
    finally:
        if job.status == "running":
            job.status = "ok" if (job.result or {}).get("ok") else "failed"
        job.finished_at = job.finished_at or time.monotonic()


def _collect_artifacts(plan: _BuildPlan, output: str) -> dict[str, Any]:
    installed = artifacts.install_from_build(
        plan.build,
        os_name=plan.os_name,
        arch=plan.arch,
        compiler=plan.toolchain,
        compiler_version=plan.compiler_version,
        qt_version=plan.qt_version,
        qt_debug=plan.qt_debug,
        build_type=plan.build_type,
    )
    injector = installed["injector"]
    library = installed["library"]
    manifest = installed["manifest"]
    return {
        "ok": True,
        "status": "ok",
        "build_id": plan.build_id,
        "build_dir": str(plan.build),
        "artifact_dir": str(installed["artifact_dir"]),
        "injector": str(injector) if injector else None,
        "library": str(library) if library else None,
        "manifest": str(manifest) if manifest else None,
        "warning": None if manifest else "build has no injector/library",
        "qt_version": plan.qt_version,
        "toolchain": plan.toolchain,
        "compiler_version": plan.compiler_version,
        "build_type": plan.build_type,
        "output": output[-4000:],
    }


def _qt_commander_source_dir() -> Path | None:
    """Return the qt-commander CMake project shipped next to this package."""
    candidate = Path(__file__).resolve().parents[1]
    return candidate if (candidate / "CMakeLists.txt").is_file() else None


def _pick_msvc(
    tools: list[dict[str, Any]], preferred_version: str | None = None
) -> dict[str, Any] | None:
    """Pick the MSVC installation a build should use.

    Several Visual Studio versions can be installed at once. The artifact path
    encodes the toolset, so building with a different toolset than the target
    was built with produces an artifact ``qt_attach`` cannot match.
    """
    candidates = [tool for tool in tools
                  if tool.get("name") == "msvc" and tool.get("vcvars")]
    if not candidates:
        candidates = [tool for tool in tools if tool.get("name") == "msvc"]
    if not candidates:
        return None
    if preferred_version:
        wanted = artifacts.compiler_abbrev("msvc", preferred_version)
        exact = [
            tool for tool in candidates
            if tool.get("version")
            and artifacts.compiler_abbrev("msvc", tool["version"]) == wanted
        ]
        if exact:
            return exact[0]
    return candidates[0]


def _target_build_args(pid: int) -> dict[str, Any]:
    """Derive ``qt_build`` arguments from a live target.

    Anything that cannot be observed is reported as missing instead of being
    filled with a default: a guessed Debug/Release flavour is exactly how a
    wrong injector artifact gets built.
    """
    descriptor = None
    for candidate in _descriptor_candidates():
        if _descriptor_pid(candidate) == pid:
            descriptor = str(candidate)
            break
    info = target.target_info(pid=pid, descriptor=descriptor)
    derived: dict[str, Any] = {}
    missing: list[str] = []
    if isinstance(info.get("qt_major"), int):
        derived["qt_major"] = info["qt_major"]
    else:
        missing.append("qt_major")
    if isinstance(info.get("toolchain"), str) and info["toolchain"]:
        derived["toolchain"] = info["toolchain"]
    else:
        missing.append("toolchain")
    if isinstance(info.get("arch"), str) and info["arch"]:
        derived["arch"] = info["arch"]
    else:
        missing.append("arch")
    if isinstance(info.get("debug"), bool):
        derived["build_type"] = "Debug" if info["debug"] else "Release"
    else:
        missing.append("build_type")
    if isinstance(info.get("qt_version"), str):
        derived["qt_version"] = info["qt_version"]
    if isinstance(info.get("compiler_version"), str):
        derived["compiler_version"] = info["compiler_version"]
    return {"ok": not missing, "derived": derived, "missing": missing}


def _annotate_build_result(result: dict[str, Any], for_pid: int | None) -> None:
    """Tell the caller whether the fresh artifact can be injected right away."""
    if not isinstance(result, dict) or not result.get("ok"):
        return
    if for_pid is None:
        result.setdefault("restart_required", False)
        return
    result["for_pid"] = for_pid
    try:
        info = target.target_info(pid=for_pid)
    except (ValueError, OSError):
        result["restart_required"] = True
        return
    loaded = info.get("loaded_injector") or {}
    expected = (info.get("expected_artifact") or {}).get("directory")
    artifact_dir = result.get("artifact_dir")
    if expected and artifact_dir:
        try:
            result["matches_target"] = (
                Path(expected).resolve() == Path(artifact_dir).resolve()
            )
        except OSError:
            result["matches_target"] = None
    restart = bool(loaded.get("loaded"))
    result["restart_required"] = restart
    result["next_step"] = (
        f"restart pid {for_pid}, then qt_attach(pid={for_pid}, "
        "mode=\"injection\")"
        if restart else
        f"qt_attach(pid={for_pid}, mode=\"injection\")"
    )


@mcp.tool
async def qt_build(
    source_dir: str | None = None,
    build_dir: str | None = None,
    qt_major: int | None = None,
    toolchain: str | None = None,
    arch: str | None = None,
    build_type: str | None = None,
    target: str | None = None,
    qt_env: str | None = None,
    vcvars_path: str | None = None,
    extra_args: list[str] | None = None,
    timeout_s: int = 300,
    wait_s: float = 20.0,
    for_pid: int | None = None,
) -> dict[str, Any]:
    """Configure and build the qt-commander injector/runtime with CMake.

    This builds the injection artifacts; it does not attach to or launch the
    application. Artifacts land under ``artifacts/<os>-<arch>/<compiler>-<version>/qt-<version>-<debug|release>/``
    and ``qt_attach(mode="injection")`` picks one by matching the target's OS,
    architecture, compiler family/version, Qt version and Qt debug/release.

    ``qt_build(for_pid=<pid>)`` derives ``qt_major``, ``toolchain``, ``arch``,
    ``build_type``, the Qt kit and ``vcvars_path`` from the live target; pass
    ``for_pid`` instead of assembling those arguments by hand. Values that
    cannot be observed are reported as missing rather than defaulted, because
    a guessed Debug/Release flavour produces an injector that cannot start in
    the target.

    ``source_dir`` defaults to the qt-commander checkout this server was
    started from. ``build_type`` must match the target's Qt runtime
    (Qt5Core.dll vs Qt5Cored.dll); the result reports ``matches_target``,
    ``restart_required`` and ``next_step``. Removing a loaded injector needs a
    target restart, not a rebuild.

    ``timeout_s`` is the total build budget; ``wait_s`` is how long this call
    blocks. Builds that outlive ``wait_s`` keep running in the server and
    return ``status="running"`` with a ``job_id``; poll ``qt_build_status`` or
    repeat the call with the same arguments (they are de-duplicated).
    """
    derived: dict[str, Any] = {}
    if for_pid is not None:
        try:
            resolved = await asyncio.to_thread(_target_build_args, for_pid)
        except (ValueError, OSError) as exc:
            return _error(ErrorCode.NOT_FOUND.value,
                          f"cannot inspect pid {for_pid}: {exc}")
        derived = resolved["derived"]
        if resolved["missing"]:
            return _error(
                ErrorCode.INVALID_ARGUMENT.value,
                f"cannot derive build arguments for pid {for_pid}: unknown "
                + ", ".join(resolved["missing"]),
                hint=("run qt_target_info(pid=...) and pass the missing "
                      "arguments explicitly; qt_build does not guess the Qt "
                      "debug/release flavour"),
            )
    if source_dir is None:
        default_source = _qt_commander_source_dir()
        if default_source is None:
            return _error(
                ErrorCode.BUILD_REQUIRED.value,
                "source_dir is required: this MCP server does not ship the "
                "qt-commander CMake project",
                hint="pass source_dir=<qt-commander checkout>",
            )
        source_dir = str(default_source)
    if qt_major is None:
        qt_major = derived.get("qt_major", 5)
    if toolchain is None:
        toolchain = derived.get("toolchain", "mingw")
    if arch is None:
        arch = derived.get("arch", "x64")
    if build_type is None:
        build_type = derived.get("build_type", "Debug")
    source = Path(source_dir)
    if not (source / "CMakeLists.txt").is_file():
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      f"CMakeLists.txt not found under {source}")
    if build_type not in {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      f"invalid build_type: {build_type}")
    if arch not in {"x64", "x86", "arm64"}:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      f"invalid arch: {arch}")
    if arch != "x64":
        return _error(ErrorCode.NOT_IMPLEMENTED.value,
                      "only host x64 builds are implemented")
    if not 1 <= timeout_s <= 3600:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "timeout_s must be between 1 and 3600")
    if extra_args is not None and not all(isinstance(a, str) for a in extra_args):
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "extra_args must be a list of strings")

    try:
        environments = await asyncio.to_thread(detect_qt_environments)
    except Exception as exc:
        return _map_error(exc)
    version = None
    kit = None
    if qt_env is not None:
        env_path = Path(qt_env)
        for item in environments:
            for candidate in item["kits"]:
                if Path(candidate["path"]) == env_path:
                    version, kit = item, candidate
                    break
            if kit is not None:
                break
        if kit is None:
            return _error(ErrorCode.INVALID_ARGUMENT.value,
                          f"qt_env is not a known Qt kit: {qt_env}")
    else:
        kits = [
            (item, candidate)
            for item in environments if item["major"] == qt_major
            for candidate in item["kits"] if candidate["compiler"] == toolchain
        ]
        preferred_qt = derived.get("qt_version")
        if preferred_qt:
            exact_kits = [pair for pair in kits
                          if pair[0]["version"] == preferred_qt]
            if exact_kits:
                kits = exact_kits
        if kits:
            version, kit = kits[0]
    if kit is None or version is None:
        return _error(ErrorCode.BUILD_REQUIRED.value,
                      f"no Qt {qt_major} {toolchain} kit found; run qt_environments")
    if version["major"] != qt_major:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "qt_env does not match qt_major")

    # One toolchain lookup drives both vcvars and the artifact's compiler
    # version, so the artifact name always describes the toolset actually used.
    msvc_tool: dict[str, Any] | None = None
    if toolchain == "msvc":
        tools = await asyncio.to_thread(detect_toolchains)
        msvc_tool = _pick_msvc(tools, derived.get("compiler_version"))
        if not vcvars_path:
            if msvc_tool is None or not msvc_tool.get("vcvars"):
                return _error(
                    ErrorCode.BUILD_REQUIRED.value,
                    "no MSVC vcvars environment found",
                    hint="install Visual Studio C++ tools or pass vcvars_path",
                )
            vcvars_path = msvc_tool["vcvars"]
        if not Path(vcvars_path).is_file():
            return _error(ErrorCode.INVALID_ARGUMENT.value,
                          f"vcvars_path not found: {vcvars_path}")

    # Resolve the compiler *before* naming the artifact directory: the name must
    # describe the compiler this build actually uses, not whatever is on PATH.
    compilers: tuple[str, str] | None = None
    compiler_version = None
    if toolchain == "msvc":
        if msvc_tool is not None and msvc_tool.get("version"):
            compiler_version = msvc_tool["version"]
    else:
        if toolchain == "mingw":
            compilers = _mingw_compilers()
            if compilers is None:
                return _error(ErrorCode.BUILD_REQUIRED.value,
                              "gcc/g++ not found for the mingw toolchain")
        compiler_version = artifacts.compiler_version(
            toolchain, compilers[1] if compilers else None
        )
    qt_bin = Path(kit["path"]) / "bin"
    qt_debug = any(qt_bin.glob(f"Qt{qt_major}Cored.*"))
    os_name = artifacts.os_slug()
    artifact_dir = artifacts.build_dir(
        os_name=os_name,
        arch=arch,
        compiler=toolchain,
        compiler_version=compiler_version,
        qt_version=version["version"],
        qt_debug=qt_debug,
    )
    build_id = f"qt{version['version']}/{toolchain}-{arch}/{build_type}"
    build = (
        Path(build_dir)
        if build_dir
        else source.parent / "build" / (
            f"{os_name}-{arch}_"
            f"{artifacts.compiler_slug(toolchain, compiler_version)}_"
            f"{artifacts.qt_slug(version['version'], qt_debug)}"
        )
    )
    cache = build / "CMakeCache.txt"
    if build_dir is None and cache.is_file():
        cached_home = _cmake_cache_home(cache)
        if cached_home and Path(cached_home).resolve() != source.resolve():
            # The default build dir is keyed by Qt/toolchain only, so a
            # previous project may have claimed it. Isolate instead of
            # deleting someone else's build tree.
            digest = hashlib.sha1(str(source.resolve()).encode("utf-8")).hexdigest()
            build = build.with_name(f"{build.name}_{digest[:8]}")
    build.mkdir(parents=True, exist_ok=True)
    cmake = shutil.which("cmake")
    if not cmake:
        return _error(ErrorCode.BUILD_FAILED.value, "cmake is not on PATH")
    ninja = shutil.which("ninja")

    configure = [
        cmake, "-S", str(source), "-B", str(build),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DQT_MAJOR_VERSION={qt_major}",
        f"-DQt{qt_major}_DIR={Path(kit['path']) / 'lib' / 'cmake' / f'Qt{qt_major}'}",
    ]
    if ninja:
        configure += ["-G", "Ninja"]
    if toolchain == "mingw" and compilers is not None:
        configure += [f"-DCMAKE_C_COMPILER={compilers[0]}",
                      f"-DCMAKE_CXX_COMPILER={compilers[1]}"]
    configure += list(extra_args or [])

    configure_command = (
        _msvc_command(vcvars_path, configure, build / "qtc-configure.bat")
        if toolchain == "msvc" and vcvars_path
        else configure
    )

    build_command = [cmake, "--build", str(build)]
    if target:
        build_command += ["--target", target]
    if toolchain == "msvc" and vcvars_path:
        build_command = _msvc_command(vcvars_path, build_command,
                                      build / "qtc-build.bat")

    plan = _BuildPlan(
        configure_command=configure_command,
        build_command=build_command,
        build=build,
        build_id=build_id,
        artifact_dir=artifact_dir,
        os_name=os_name,
        arch=arch,
        toolchain=toolchain,
        compiler_version=compiler_version,
        qt_version=version["version"],
        qt_debug=qt_debug,
        build_type=build_type,
        timeout_s=timeout_s,
    )
    key = _job_key(plan)
    job: _BuildJob | None = None
    running_id = _build_jobs_by_key.get(key)
    if running_id:
        candidate = _build_jobs.get(running_id)
        if candidate is not None and candidate.status == "running":
            job = candidate
    if job is None:
        job = _BuildJob(job_id=secrets.token_hex(4), key=key, plan=plan)
        _build_jobs[job.job_id] = job
        _build_jobs_by_key[key] = job.job_id
        job.task = asyncio.create_task(_run_build_job(job))
        # Keep the registry bounded; finished jobs are still queryable briefly.
        if len(_build_jobs) > 16:
            for stale in sorted(_build_jobs.values(),
                                key=lambda item: item.started_at)[:-8]:
                if stale.status != "running":
                    _build_jobs.pop(stale.job_id, None)
                    _build_jobs_by_key.pop(stale.key, None)

    if wait_s > 0 and job.task is not None and job.status == "running":
        await asyncio.wait({job.task}, timeout=wait_s)
    if job.status == "running":
        return {
            "ok": True,
            "status": "running",
            "job_id": job.job_id,
            "build_id": plan.build_id,
            "build_dir": str(plan.build),
            "artifact_dir": str(plan.artifact_dir),
            "stage": job.stage,
            "elapsed_s": round(time.monotonic() - job.started_at, 1),
            "for_pid": for_pid,
            "hint": "build continues in the server; call qt_build_status("
                    "job_id=...) or repeat qt_build with the same arguments",
        }
    if job.result is None:
        return _build_result(plan, ok=False, stage=job.stage,
                             message=f"build {job.status}")
    job.result.setdefault("job_id", job.job_id)
    job.result.setdefault("kit", kit["path"])
    _annotate_build_result(job.result, for_pid)
    return job.result


@mcp.tool
async def qt_build_status(job_id: str | None = None) -> dict[str, Any]:
    """Report background build jobs started by qt_build.

    Pass ``job_id`` for one job; without it, returns running jobs first and
    then the most recent finished ones (max 5).
    """
    if job_id is not None:
        job = _build_jobs.get(job_id)
        if job is None:
            return _error(ErrorCode.NOT_FOUND.value,
                          f"unknown build job: {job_id}")
        return {"ok": True, "jobs": [_job_summary(job)]}
    jobs = sorted(_build_jobs.values(), key=lambda item: item.started_at,
                  reverse=True)
    running = [job for job in jobs if job.status == "running"]
    finished = [job for job in jobs if job.status != "running"]
    return {
        "ok": True,
        "running": len(running),
        "jobs": [_job_summary(job) for job in (running + finished)[:5]],
    }


@mcp.tool
async def qt_attach(
    descriptor: str | None = None,
    pid: int | None = None,
    session_id: str | None = None,
    mode: str = "auto",
    build_id: str | None = None,
    artifact_dir: str | None = None,
) -> dict[str, Any]:
    """Attach to a live Qt process.

    Use this before any session-scoped tool. Use a rendezvous descriptor
    (linked mode), or ``pid`` with ``mode="injection"``. ``mode="auto"`` uses a
    live descriptor when available and otherwise injects. ``artifact_dir``
    pins one artifact directory when several match the target.

    Failures are self-describing: ``error.reason`` is one of
    ``injector_already_loaded``, ``artifact_missing``, ``artifact_ambiguous``,
    ``qt_runtime_flavor_mismatch``, ``timeout_waiting_qcoreapp``,
    ``target_not_ready``, ``permission_denied`` or ``injection_failed``, and
    ``error.details`` carries the target snapshot, the chosen artifact, the
    already-loaded injector and the artifact ``qt_attach`` expected.
    ``error.restart_required`` means the target must be restarted before an
    injector can be loaded again; a rebuild alone cannot replace a library that
    is already mapped into the process.

    ``build_id`` is reserved: it is accepted but not validated yet, so it
    always returns ``E_NOT_IMPLEMENTED``. Check
    ``qt_target_info.loaded_injector`` / ``expected_artifact`` instead.
    """
    if mode not in {"auto", "linked", "injection"}:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      f"unsupported attach mode: {mode}")
    if artifact_dir is not None:
        directory = Path(artifact_dir)
        if not (directory / "manifest.json").is_file():
            return _error(ErrorCode.INVALID_ARGUMENT.value,
                          f"artifact_dir has no manifest.json: {directory}")
    _gc_descriptors()
    if mode == "injection" and descriptor:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "descriptor cannot be combined with injection mode")
    if mode == "injection" and pid is None:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "injection mode requires pid")
    if build_id is not None:
        return _error(
            ErrorCode.NOT_IMPLEMENTED.value,
            "build_id attach is not implemented",
            hint="compare qt_target_info.loaded_injector.build_id with "
                 "expected_artifact.build_id instead",
        )

    if mode == "injection":
        existing = _live_descriptors(pid)
        if existing:
            return _error(
                ErrorCode.MODE_CONFLICT.value,
                "target already has a descriptor",
                hint="attach with qt_attach(pid=...) or "
                     "qt_attach(descriptor=...) instead of mode=\"injection\"",
            )
        injected = await _inject_target(pid, artifact_dir)
        if isinstance(injected, dict):
            return injected
        path = injected
    elif descriptor:
        path = Path(descriptor)
        if session_id is not None and _descriptor_session_id(path) != session_id:
            return _error(ErrorCode.LINKED_IDENTITY.value,
                          "descriptor session mismatch")
    else:
        candidates = _live_descriptors(pid)
        if session_id is not None:
            candidates = [p for p in candidates
                          if _descriptor_session_id(p) == session_id]
        if not candidates:
            if mode == "auto" and pid is not None:
                injected = await _inject_target(pid, artifact_dir)
                if isinstance(injected, dict):
                    return injected
                path = injected
            else:
                return _error(
                    ErrorCode.LINKED_NOT_FOUND.value,
                    "no linked descriptor found",
                    hint="start the app with qt_commander::start() or pass mode=injection",
                )
        else:
            if len(candidates) > 1:
                return _error(
                    ErrorCode.AMBIGUOUS_SESSION.value,
                    "multiple linked descriptors found; pass descriptor or pid",
                )
            path = candidates[0]
    if not path.exists() or not path.is_file():
        return _error(
            ErrorCode.LINKED_NOT_FOUND.value,
            f"descriptor not found: {path}",
        )
    if path.is_symlink():
        return _error(ErrorCode.RENDEZVOUS_UNSAFE.value,
                      "descriptor must not be a symlink")
    try:
        if path.stat().st_size > 64 * 1024:
            return _error(ErrorCode.LINKED_INVALID.value,
                          "descriptor is too large")
        if os.name != "nt" and (path.stat().st_mode & 0o077):
            return _error(ErrorCode.RENDEZVOUS_UNSAFE.value,
                          "descriptor permissions are too broad")
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _error(ErrorCode.LINKED_INVALID.value, str(exc))
    if not isinstance(data, dict):
        return _error(ErrorCode.LINKED_INVALID.value,
                      "descriptor root must be an object")
    protocol = data.get("protocol")
    if not isinstance(protocol, dict) or protocol.get("major") != 2:
        return _error(ErrorCode.PROTOCOL.value,
                      "unsupported descriptor protocol")
    if data.get("schema") != 2:
        return _error(ErrorCode.LINKED_INVALID.value, "unsupported descriptor schema")
    if data.get("mode") not in {"linked", "injection"}:
        return _error(ErrorCode.LINKED_INVALID.value,
                      "descriptor mode must be linked or injection")
    try:
        descriptor_pid = int(data["pid"])
        port = int(data["port"])
    except (KeyError, TypeError, ValueError) as exc:
        return _error(ErrorCode.LINKED_INVALID.value, str(exc))
    token = data.get("token")
    if not isinstance(token, str) or not token:
        return _error(ErrorCode.LINKED_INVALID.value, "descriptor token is missing")
    if not 1 <= port <= 65535:
        return _error(ErrorCode.LINKED_INVALID.value, "descriptor port is invalid")
    session_id = data.get("session_id")
    if not isinstance(session_id, str) or not session_id:
        return _error(ErrorCode.LINKED_INVALID.value,
                      "descriptor session_id is missing")
    if pid is not None and descriptor_pid != pid:
        return _error(ErrorCode.LINKED_IDENTITY.value, "descriptor pid mismatch")

    start_time = data.get("process_start_time")
    if start_time is not None:
        try:
            expected_start = float(start_time)
            actual_start = psutil.Process(descriptor_pid).create_time()
        except (TypeError, ValueError, psutil.Error):
            return _error(ErrorCode.LINKED_STALE.value,
                          "descriptor process identity is stale")
        if abs(expected_start - actual_start) > 0.25:
            return _error(ErrorCode.LINKED_STALE.value,
                          "descriptor process start time mismatch")

    try:
        existing = sessions.get(session_id)
    except SessionNotFound:
        existing = None
    if existing is not None:
        if existing.connected:
            return _error(ErrorCode.SESSION_EXISTS.value,
                          f"session already attached: {session_id}")
        sessions.remove(session_id)
        await existing.close()

    client = None
    try:
        client = await RpcClient.connect("127.0.0.1", port, token)
        if client.hello is None or client.hello.session_tag != session_id:
            await client.close()
            return _error(ErrorCode.LINKED_IDENTITY.value,
                          "descriptor and runtime identity mismatch")
        session = Session(session_id, descriptor_pid, client, client.hello,
                          build_id=data.get("library_build_id"),
                          descriptor=str(path))
        sessions.add(session)
    except SessionExists:
        if client is not None:
            await client.close()
        return _error(ErrorCode.SESSION_EXISTS.value, session_id)
    except (ClientTimeout, ClientClosed, RpcError, OSError, ValueError) as exc:
        if client is not None:
            await client.close()
        return _map_error(exc)

    return {
        "ok": True,
        "session_id": session_id,
        "pid": session.pid,
        "capabilities": list(session.hello.capabilities),
        "descriptor": str(path),
    }


def _launch_library(explicit: str | None) -> tuple[Path | None, dict[str, Any] | None]:
    """Pick the library to preload: explicit path, env override, or one artifact."""
    if explicit:
        path = Path(explicit).expanduser()
        if path.is_file():
            return path, None
        return None, _error(ErrorCode.INVALID_ARGUMENT.value,
                            f"library not found: {path}")
    override = os.environ.get("QT_COMMANDER_INJECT_DLL")
    if override:
        path = Path(override).expanduser()
        if path.is_file():
            return path, None
        return None, _error(
            ErrorCode.INVALID_ARGUMENT.value,
            f"QT_COMMANDER_INJECT_DLL is not a file: {path}",
        )
    wanted_os = artifacts.os_slug()
    wanted_arch = artifacts.host_arch()
    matches: list[tuple[Path, str | None]] = []
    for entry in artifacts.listing():
        manifest = entry.get("manifest") or {}
        scope = manifest.get("target") or {}
        if scope.get("os") != wanted_os or scope.get("arch") != wanted_arch:
            continue
        relative = manifest.get("library")
        if not isinstance(relative, str):
            continue
        path = Path(entry["directory"]) / relative
        if path.is_file():
            matches.append((path, entry.get("build_id")))
    if len(matches) == 1:
        return matches[0][0], None
    if not matches:
        return None, _error(
            ErrorCode.BUILD_REQUIRED.value,
            f"no injection library for {wanted_os}/{wanted_arch}",
            hint="pass library=<path>, set QT_COMMANDER_INJECT_DLL, or build one "
                 "with qt_build for a running target",
        )
    result = _error(
        ErrorCode.AMBIGUOUS.value,
        "several injection libraries match this host",
        hint="pass library=<one of details.candidates>",
    )
    result["error"]["details"] = {
        "candidates": [
            {"library": str(path), "build_id": build_id}
            for path, build_id in matches
        ]
    }
    return None, result


def _launch_timeout_hint(pid: int, library: Path) -> str:
    parts = [
        f"pid {pid} started with {library.name} preloaded but no runtime "
        "appeared",
        "the library must match the app's Qt version, debug/release flavour "
        "and architecture",
    ]
    if launcher.platform_key() == "darwin":
        parts.append(
            "on macOS a hardened app ignores DYLD_INSERT_LIBRARIES unless the "
            "dylib is signed by the same Team and library validation is "
            "disabled"
        )
    return "; ".join(parts)


async def qt_launch(
    program: str,
    args: list[str] | None = None,
    library: str | None = None,
    cwd: str | None = None,
    env: dict[str, str] | None = None,
    timeout_ms: int = 20000,
    detach: bool = True,
) -> dict[str, Any]:
    """Start an application with the runtime preloaded, then attach to it.

    Linux and macOS only. The dynamic loader inserts the library before
    ``main`` (``LD_PRELOAD`` on Linux, ``DYLD_INSERT_LIBRARIES`` on macOS), so
    the runtime exists before the first window does and nothing is injected
    into a live process. That is also the only workable approach on macOS,
    which refuses injection into an already-running hardened or notarized
    process.

    ``program`` may be an executable or a macOS ``.app`` bundle (the inner
    executable is resolved from ``Contents/Info.plist``). ``args`` are passed
    through; ``env`` is merged into the child's environment after the preload
    variable is set.

    ``library`` is the injector library to preload. Omit it to use
    ``QT_COMMANDER_INJECT_DLL``, or the single installed artifact whose OS and
    architecture match this host.

    The tool waits up to ``timeout_ms`` for the runtime's descriptor, then
    attaches and returns the attach payload plus ``launched`` (pid, library,
    waited_ms). A process that starts but never registers returns
    ``E_TARGET_NOT_QT`` with the pid, so it can still be inspected. This tool
    is not registered on Windows, which has no preload mechanism; use
    ``qt_attach(pid=..., mode="injection")`` there.
    """
    if not launcher.supported():
        return _error(
            ErrorCode.NOT_IMPLEMENTED.value,
            f"launch injection is not available on {launcher.platform_key()}",
            hint="use qt_attach(pid=..., mode=\"injection\") on Windows",
        )
    if args is not None and not all(isinstance(arg, str) for arg in args):
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "args must be a list of strings")
    if env is not None and not all(
        isinstance(key, str) and isinstance(value, str)
        for key, value in env.items()
    ):
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "env must map strings to strings")
    if not 500 <= timeout_ms <= 120000:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "timeout_ms must be between 500 and 120000")
    resolved, failure = _launch_library(library)
    if failure is not None:
        return failure
    if resolved is None:  # pragma: no cover - _launch_library always sets one
        return _error(ErrorCode.INVALID_ARGUMENT.value, "no library resolved")
    started = time.monotonic()
    try:
        process = await asyncio.to_thread(
            launcher.launch,
            program,
            args or [],
            library=resolved,
            cwd=cwd,
            env=env,
            detach=detach,
        )
    except launcher.LaunchError as exc:
        return _error(exc.code, str(exc), hint=exc.hint)
    if detach:
        # The child is ours: reap it off the event loop so a finished app does
        # not linger as a zombie.
        threading.Thread(target=process.wait, daemon=True).start()
    descriptor = await _await_descriptor_for_pid(
        process.pid, timeout_s=timeout_ms / 1000.0
    )
    if descriptor is None:
        result = _error(
            ErrorCode.TARGET_NOT_QT.value,
            f"pid {process.pid} started but no runtime registered",
            hint=_launch_timeout_hint(process.pid, resolved),
            retryable=False,
        )
        result["error"]["details"] = {
            "pid": process.pid,
            "library": str(resolved),
            "waited_ms": int((time.monotonic() - started) * 1000),
        }
        return result
    attached = await qt_attach(descriptor=str(descriptor))
    if not attached.get("ok"):
        attached.setdefault("error", {})["details"] = {
            "pid": process.pid,
            "library": str(resolved),
        }
        return attached
    attached["launched"] = {
        "pid": process.pid,
        "library": str(resolved),
        "waited_ms": int((time.monotonic() - started) * 1000),
    }
    return attached


if LAUNCH_SUPPORTED:
    # Windows has no preload hook. Registering the tool there would advertise a
    # capability the platform cannot deliver, so the tool simply does not exist.
    mcp.add_tool(qt_launch)


@mcp.tool
async def qt_detach(session_id: str, purge: bool = False) -> dict[str, Any]:
    """Close a session; ``purge=True`` also removes its descriptor file.

    Detach before rebuilding/reinjecting a library if the old DLL is loaded.
    """
    try:
        session = sessions.remove(session_id)
    except SessionNotFound:
        return _error(ErrorCode.SESSION_EXPIRED.value,
                      f"unknown session: {session_id}")
    await session.close()
    purged = False
    if purge and session.descriptor:
        try:
            Path(session.descriptor).unlink(missing_ok=True)
            purged = True
        except OSError:
            purged = False
    _gc_descriptors()
    return {"ok": True, "session_id": session_id, "purged": purged}


@mcp.tool
async def qt_query(
    session_id: str,
    where: dict[str, Any] | None = None,
    scope: dict[str, Any] | None = None,
    props: Any = None,
    coords: str = "parent",
    max_depth: int = -1,
    limit: int = 200,
    include_hidden: bool = False,
    occlusion: bool = False,
    count_only: bool = False,
    compact: bool = False,
) -> dict[str, Any]:
    """Query the live object tree.

    Use when the target is unknown, or to survey a subtree. For one already
    known target, prefer qt_get. ``where`` accepts
    id/class/inherits/object_name/objectName/qml_id/qml_type/text/
    props; strings support exact, wildcard, and contains/startsWith/regex/
    equals matchers. ``scope`` limits to a window or subtree. Without
    ``props``, nodes include text/visible/enabled. ``limit`` is a soft
    truncation reported via count/total/limit/truncated. A filtered miss that
    only matches hidden nodes returns ``count:0`` plus ``hidden_matched`` and a
    hint. ``compact`` omits default-valued fields (children/child_count/
    qml_id/qml_type/window) to save tokens.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.query(
            session,
            where=where,
            scope=scope,
            props=props,
            coords=coords,
            max_depth=max_depth,
            limit=limit,
            include_hidden=include_hidden,
            occlusion=occlusion,
            count_only=count_only,
            compact=compact,
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_items(
    owner: str | dict[str, Any],
    where: dict[str, Any] | None = None,
    limit: int = 50,
    offset: int = 0,
    include_hidden: bool = False,
    compact: bool = False,
    nth: int | None = None,
    session_id: str | None = None,
) -> dict[str, Any]:
    """List provider-backed children of a widget/QML owner.

    Use when the regular query tree lacks actions, tabs, cells, dock/toolbar
    items, or splitter handles. Covers actions, tabs, table/list/tree cells,
    dock/toolbar items, splitter
    handles, and QML item children. These children are virtual and are not
    part of the QWidget query tree. ``owner`` accepts a node id or a locator
    (class/object_name/text/props) like the other target parameters; use
    ``nth`` when the locator is ambiguous.
    """
    hint_id = owner if isinstance(owner, str) else ""
    resolved = session_id or (hint_id.split(".")[0] if hint_id else None)
    if resolved is None:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "session_id is required when owner is a locator")
    session = _session(resolved)
    if isinstance(session, dict):
        return session
    try:
        return await ops.items(
            session,
            owner,
            where=where,
            limit=limit,
            offset=offset,
            include_hidden=include_hidden,
            compact=compact,
            nth=nth,
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_act(
    session_id: str,
    target: str | dict[str, Any],
    action: str,
    nth: int | None = None,
    position: dict[str, float] | None = None,
    geometry: dict[str, float] | None = None,
    button: str = "left",
    modifiers: list[str] | None = None,
    text: str | None = None,
    clear: bool = False,
    submit: bool = False,
    keys: list[str] | None = None,
    dx: float = 0.0,
    dy: float = 0.0,
    to: str | dict[str, Any] | None = None,
    duration_ms: int = 300,
    expect: dict[str, Any] | list[dict[str, Any]] | None = None,
    timeout_ms: int = 2000,
    settle_ms: int = 0,
    wait_ms: int = 1000,
    observe: bool = True,
    input: str = "auto",
    include_hidden: bool = False,
    js_risk_ack: bool = False,
) -> dict[str, Any]:
    """Dispatch one real input or window action.

    Use for user-like input; use qt_set for property writes and qt_call for
    slots. Actions: click/dblclick/hover/type/focus/key/scroll/drag, plus
    minimize/restore/set_geometry for window targets (set_geometry takes
    ``geometry={x,y,w,h}``). ``target`` can be a node id, a string QML id, or
    an object with id/qml_id/text/class/objectName; use ``nth`` for
    ambiguity. Hidden targets require ``include_hidden=true``. ``expect`` verifies a
    declarative state; ``type`` accepts clear/submit.

    ``wait_ms`` (default 1000) is how long the call may stay blocked inside the
    target before it returns ``{"state": "running", "task_id": ...}`` — for
    example when the click opened a modal dialog. Use qt_task with that id to
    collect the final result; the action has already been dispatched, so never
    retry it blindly.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.act(
            session,
            target,
            action,
            nth=nth,
            position=position,
            geometry=geometry,
            button=button,
            modifiers=modifiers,
            text=text,
            clear=clear,
            submit=submit,
            keys=keys,
            dx=dx,
            dy=dy,
            to=to,
            duration_ms=duration_ms,
            expect=expect,
            timeout_ms=timeout_ms,
            settle_ms=settle_ms,
            wait_ms=wait_ms,
            observe=observe,
            input=input,
            include_hidden=include_hidden,
            js_risk_ack=js_risk_ack,
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_batch(
    session_id: str,
    steps: list[dict[str, Any]],
    stop_on_error: bool = True,
    timeout_ms: int = 30000,
    max_steps: int = 50,
) -> dict[str, Any]:
    """Run steps sequentially within one budget.

    Each step is exactly one of {"act": {...}}, {"wait": {...}},
    {"set": {...}}, or {"get": {...}}. Useful for write/read/restore
    sequences; ``stop_on_error`` is true by default. The whole batch shares
    ``timeout_ms`` (100..60000): waits are clamped to the remaining time, and
    an exhausted budget returns the completed steps with
    ``cause="batch_timeout"`` instead of losing the whole call. ``max_steps``
    bounds the sequence length. If a step is still running in the target
    (``state: "running"``), the batch stops at once with ``cause="pending"``
    plus that step's ``task_id``; collect it with qt_task before continuing.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.batch(
            session,
            steps,
            stop_on_error=stop_on_error,
            timeout_ms=timeout_ms,
            max_steps=max_steps,
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_wait(
    session_id: str,
    condition: dict[str, Any],
    timeout_ms: int = 2000,
    poll_ms: int = 75,
) -> dict[str, Any]:
    """Poll a declarative condition until it matches or times out.

    A condition may use where/props/gone selectors. Timeouts are reported as
    matched=false rather than silently succeeding.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.wait(
            session, condition, timeout_ms=timeout_ms, poll_ms=poll_ms
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_task(
    session_id: str,
    task_id: str,
    wait_ms: int = 0,
) -> dict[str, Any]:
    """Read the result of an asynchronous task.

    ``qt_act``/``qt_call``/``qt_set`` return ``{"state": "running",
    "task_id": ...}`` when the call is still executing inside the target after
    its ``wait_ms`` budget. Pass that id here to collect the result.
    ``state`` stays ``running`` while the work is in progress and becomes
    ``done`` once the result is available (the payload is under ``result``).
    A running task must not be retried: the call may already have had side
    effects. ``wait_ms`` on this tool only polls; it never starts work.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.task(session, task_id, wait_ms=wait_ms)
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_screenshot(
    session_id: str,
    target: str | dict[str, Any] | None = None,
    nth: int | None = None,
    max_width: int = 1024,
    annotate: str = "none",
    where: dict[str, Any] | None = None,
    max_labels: int = 50,
    annotate_offset: int = 0,
    format: str = "png",
) -> list[Any]:
    """Capture a PNG plus metadata.

    Use for visual evidence and annotation; use the returned legend ids as
    action targets. With no target, exactly one visible top-level window is
    captured; if
    several are visible the call fails with ``E_AMBIGUOUS`` and the caller
    must choose a window from ``qt_windows``.

    With ``annotate`` enabled, ``legend`` lists numbered regions
    (``n``, ``id``, ``rect``, ``image_rect``). The number is only a
    screenshot-local marker; use ``id`` from the legend to act.
    ``annotate_offset`` pages large annotation sets; legend numbers stay
    continuous. ``max_width`` controls output width.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return [json.dumps(session, ensure_ascii=False)]
    try:
        frame = await ops.screenshot(
            session,
            target=target,
            nth=nth,
            max_width=max_width,
            annotate=annotate,
            where=where,
            max_labels=max_labels,
            annotate_offset=annotate_offset,
            format=format,
        )
    except Exception as exc:
        return [json.dumps(_map_error(exc), ensure_ascii=False)]
    metadata = dict(frame.result)
    metadata["ok"] = True
    metadata["bytes"] = len(frame.binary)
    return [Image(data=frame.binary, format="png"),
            json.dumps(metadata, ensure_ascii=False)]


@mcp.tool
async def qt_get(
    session_id: str,
    target: str | dict[str, Any],
    props: Any = None,
    nth: int | None = None,
    include_hidden: bool = False,
) -> dict[str, Any]:
    """Read properties and identity for one node or virtual item.

    Use after an action to verify state, or when the target is already known;
    use qt_query for discovery. ``props`` may be a list of names or "*" for
    all readable properties.
    Hidden QML targets require ``include_hidden=true``.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.get(session, target, props=props, nth=nth,
                             include_hidden=include_hidden)
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_set(
    session_id: str,
    target: str | dict[str, Any],
    name: str,
    value: Any,
    nth: int | None = None,
    include_hidden: bool = False,
    wait_ms: int = 3000,
) -> dict[str, Any]:
    """Write one property (destructive).

    Use for property setters such as
    QPushButton::setText, which are not qt_call methods. Hidden QML targets
    require ``include_hidden=true``. ``wait_ms`` bounds how long the call may
    block before it returns a ``task_id`` (0 = return as soon as it is queued).
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.set_property(session, target, name, value, nth=nth,
                                      include_hidden=include_hidden,
                                      wait_ms=wait_ms)
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_call(
    session_id: str,
    target: str | dict[str, Any],
    method: str,
    args: list[Any] | None = None,
    nth: int | None = None,
    signature: str | None = None,
    include_hidden: bool = False,
    wait_ms: int = 3000,
) -> dict[str, Any]:
    """Invoke a Qt metaobject method (destructive).

    Use when you need a slot or Q_INVOKABLE method; use qt_set for property
    setters and qt_act for user-like input. QWidget and QML targets are both
    supported. Only slots/Q_INVOKABLE are
    callable; property setters are not methods. Use qt_methods to discover
    signatures, then pass ``signature`` to disambiguate overloads. Enum
    arguments accept their integer values. Non-void results are returned in
    ``result``; object results are ``{class, id}`` and are reusable targets.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.call_method(session, target, method, args=args, nth=nth,
                                     signature=signature,
                                     include_hidden=include_hidden,
                                     wait_ms=wait_ms)
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_methods(
    session_id: str,
    target: str | dict[str, Any],
    nth: int | None = None,
    include_hidden: bool = False,
    include_internal: bool = False,
) -> dict[str, Any]:
    """List invokable Qt methods (slots/Q_INVOKABLE) for a target.

    Use before qt_call when the method name or signature is unknown.
    Returns name/signature/parameter_types/return_type/slot. Qt ``_q_``
    internal methods are filtered unless ``include_internal=true``.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    params: dict[str, Any] = {"target": target}
    if nth is not None:
        params["nth"] = nth
    if include_hidden:
        params["include_hidden"] = True
    if include_internal:
        params["include_internal"] = True
    try:
        return await session.call("methods", params)
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_source(
    session_id: str,
    target: str | dict[str, Any],
    handler: str | None = None,
    snippet_lines: int = 3,
    max_lines: int = 20,
    nth: int | None = None,
    include_hidden: bool = False,
    source_dir: str | None = None,
) -> dict[str, Any]:
    """Resolve a widget/QML target to source.

    Use after a UI target is identified; this is not a free-form code search.
    Scans the target's source tree (and QT_COMMANDER_SOURCE_DIR) with a bounded
    budget (120 ms / 20k entries) and returns file/line/url plus a ``snippet``
    when found. ``source_dir`` restricts the search to exactly that directory
    (it does not add roots). A miss returns ``E_NO_DEBUGINFO``; when the budget
    runs out first the code is ``E_SOURCE_TIMEOUT`` and ``search.truncated`` is
    true, so pass ``source_dir`` to narrow the scan.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.source(
            session,
            target,
            handler=handler,
            snippet_lines=snippet_lines,
            max_lines=max_lines,
            nth=nth,
            include_hidden=include_hidden,
            source_dir=source_dir,
        )
    except Exception as exc:
        return _map_error(exc)


class _ActionableErrors(Middleware):
    """Turn schema/validation failures into the same error envelope as tools.

    Without this, a missing ``session_id`` surfaces as a raw pydantic
    validation error from the MCP layer instead of ``{"ok": false, "error":
    {"code": "E_INVALID_ARGUMENT"}}``.
    """

    async def on_call_tool(self, context, call_next):
        try:
            return await call_next(context)
        except ToolValidationError as exc:
            name = getattr(context.message, "name", "tool")
            first_line = str(exc).splitlines()[0][:200] if str(exc) else ""
            return ToolResult(
                structured_content=_error(
                    ErrorCode.INVALID_ARGUMENT.value,
                    f"invalid arguments for {name}",
                    hint=first_line,
                ),
                is_error=False,
            )


mcp.add_middleware(_ActionableErrors())


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()



