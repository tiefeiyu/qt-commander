"""qt-commander MCP server (linked mode vertical slice)."""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any

import psutil
from fastmcp import FastMCP
from fastmcp.utilities.types import Image

from . import inject, ops, target
from .client import ClientClosed, ClientTimeout, RpcClient, RpcError
from .environment import detect_qt_environments, detect_toolchains
from .errors import ErrorCode
from .session import Session, SessionExists, SessionManager, SessionNotFound

INSTRUCTIONS = """\
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
   architecture, Qt major version, and toolchain. Use qt_build for the
   matching injector/runtime. If attach rejects a build/architecture mismatch,
   rebuild; do not bypass it.
3. qt_attach(pid=..., mode="injection"), then qt_windows to choose a window.
   Keep one session per process.
4. qt_query to locate a target; then act/read/write/call.
5. Verify with query/get/screenshot, close popups you opened, and qt_detach.

Critical rules:
- The tree is live; re-query when a target may be stale. A successful act is
  not proof; verify the resulting state.
- Targets: returned id, string QML id, or {"qml_id":...}/{"text":...}/
  {"class":...}/{"object_name":...}. Combine fields for repeated delegates;
  use nth for ambiguity.
- E_AMBIGUOUS: inspect candidates. E_HIDDEN: hidden by design; use
  include_hidden=true only when intentional.
- qt_call only invokes slots/Q_INVOKABLE; property setters use qt_set. Use
  qt_methods and signature= for overloads.
- On timeout or failure, inspect first. Retry at most once with a changed
  query/wait, then stop and report.
- Confirm before irreversible actions (delete/upload/exit/restart or
  persistent setting changes). Do not bypass hidden/safety checks.
"""

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


async def _await_descriptor_for_pid(pid: int, timeout_s: float = 20.0) -> Path | None:
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
        await asyncio.sleep(0.1)
    return None


async def _inject_target(pid: int) -> Path | dict[str, Any]:
    dll = inject.default_dll_path()
    if dll is None:
        return _error(
            ErrorCode.INJECTION_FAILED.value,
            "injection DLL not found",
            hint="build qt_commander_inject or set QT_COMMANDER_INJECT_DLL",
        )
    status_path = Path(tempfile.gettempdir()) / f"qtc-inject-status-{pid}.txt"
    try:
        status_path.unlink(missing_ok=True)
    except OSError:
        pass
    try:
        await asyncio.to_thread(inject.inject_library, pid, dll)
    except inject.InjectionError as exc:
        return _error(exc.code, str(exc))
    path = await _await_descriptor_for_pid(pid)
    if path is None:
        message = ""
        try:
            message = status_path.read_text(encoding="utf-8").strip()
        except OSError:
            pass
        if message and message != "ok":
            return _error(ErrorCode.INJECTION_FAILED.value, message)
        return _error(
            ErrorCode.TARGET_NOT_QT.value,
            "DLL loaded but no descriptor appeared; target may not be a Qt app",
        )
    return path


@mcp.tool
async def qt_status(session_id: str | None = None) -> dict[str, Any]:
    """Return live sessions/descriptors, optionally probing one session.

    Pass ``session_id`` to probe an existing session; otherwise this lists
    descriptor files and their process liveness.
    """
    candidates = _descriptor_candidates()
    result: dict[str, Any] = {
        "sessions": [asdict(info) for info in sessions.list()],
        "descriptors": [str(path) for path in candidates],
        "descriptor_info": [_descriptor_summary(path) for path in candidates],
        "descriptor": str(candidates[0]) if len(candidates) == 1 else None,
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
    executable image, loader PATH, Qt runtime DLLs, and descriptor build id;
    it does not inject or launch anything. ``qt_version`` is the full version
    (for example 5.15.2) when the descriptor or PE version resource provides
    it; ``build_type`` comes from the descriptor when available. Without
    pid/descriptor it uses the sole live descriptor.
    """
    if pid is None and descriptor is None:
        candidates = _descriptor_candidates()
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
    paginate. Returned ids are valid targets for this session.
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


@mcp.tool
async def qt_environments() -> dict[str, Any]:
    """Detect Qt kits and C++ toolchains available on this machine.

    Read-only; use it before qt_build when the Qt path/toolchain is unknown.
    """
    def detect() -> dict[str, Any]:
        return {
            "qt": detect_qt_environments(),
            "toolchains": detect_toolchains(),
        }

    try:
        return await asyncio.to_thread(detect)
    except Exception as exc:
        return _map_error(exc)


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
    return process.returncode or 0, output.decode("utf-8", "replace")


@mcp.tool
async def qt_build(
    source_dir: str,
    build_dir: str | None = None,
    qt_major: int = 5,
    toolchain: str = "mingw",
    arch: str = "x64",
    build_type: str = "Debug",
    target: str | None = None,
    qt_env: str | None = None,
    vcvars_path: str | None = None,
    extra_args: list[str] | None = None,
    timeout_s: int = 300,
) -> dict[str, Any]:
    """Configure and build a CMake project against a discovered Qt kit.

    This builds a test/target runtime; it does not attach to or launch the
    application. The build must match the target process's Qt major version,
    toolchain, architecture, and build type.
    """
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
        for item in environments:
            if item["major"] != qt_major:
                continue
            for candidate in item["kits"]:
                if candidate["compiler"] == toolchain:
                    version, kit = item, candidate
                    break
            if kit is not None:
                break
    if kit is None or version is None:
        return _error(ErrorCode.BUILD_REQUIRED.value,
                      f"no Qt {qt_major} {toolchain} kit found; run qt_environments")
    if version["major"] != qt_major:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "qt_env does not match qt_major")
    if toolchain == "msvc":
        if not vcvars_path:
            tools = await asyncio.to_thread(detect_toolchains)
            msvc = next(
                (tool for tool in tools
                 if tool.get("name") == "msvc" and tool.get("vcvars")),
                None,
            )
            if msvc is None:
                return _error(
                    ErrorCode.BUILD_REQUIRED.value,
                    "no MSVC vcvars environment found",
                    hint="install Visual Studio C++ tools or pass vcvars_path",
                )
            vcvars_path = msvc["vcvars"]
        if not Path(vcvars_path).is_file():
            return _error(ErrorCode.INVALID_ARGUMENT.value,
                          f"vcvars_path not found: {vcvars_path}")

    build_id = f"qt{version['version']}/{toolchain}-{arch}/{build_type}"
    build = Path(build_dir) if build_dir else source.parent / "builds" / build_id
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
    if toolchain == "mingw":
        compilers = _mingw_compilers()
        if compilers is None:
            return _error(ErrorCode.BUILD_REQUIRED.value,
                          "gcc/g++ not found for the mingw toolchain")
        configure += [f"-DCMAKE_C_COMPILER={compilers[0]}",
                      f"-DCMAKE_CXX_COMPILER={compilers[1]}"]
    configure += list(extra_args or [])

    configure_command = (
        _msvc_command(vcvars_path, configure, build / "qtc-configure.bat")
        if toolchain == "msvc" and vcvars_path
        else configure
    )
    deadline = time.monotonic() + timeout_s
    try:
        code, output = await _run_build_command(
            configure_command, max(1, int(deadline - time.monotonic())))
    except OSError as exc:
        return _error(ErrorCode.BUILD_FAILED.value, str(exc))
    if code == -1:
        return {
            "ok": False,
            "build_id": build_id,
            "build_dir": str(build),
            "stage": "timeout",
            "error": {
                "code": ErrorCode.BUILD_FAILED.value,
                "message": "cmake configure timed out",
            },
        }
    if code != 0:
        return {
            "ok": False,
            "build_id": build_id,
            "build_dir": str(build),
            "stage": "configure",
            "error": {
                "code": ErrorCode.BUILD_FAILED.value,
                "message": "cmake configure failed",
                "details": {"exit_code": code, "output": output[-8000:]},
            },
        }

    build_command = [cmake, "--build", str(build)]
    if target:
        build_command += ["--target", target]
    if toolchain == "msvc" and vcvars_path:
        build_command = _msvc_command(vcvars_path, build_command,
                                      build / "qtc-build.bat")
    try:
        code, output = await _run_build_command(
            build_command, max(1, int(deadline - time.monotonic())))
    except OSError as exc:
        return _error(ErrorCode.BUILD_FAILED.value, str(exc))
    if code == -1:
        return {
            "ok": False,
            "build_id": build_id,
            "build_dir": str(build),
            "stage": "timeout",
            "error": {
                "code": ErrorCode.BUILD_FAILED.value,
                "message": "cmake build timed out",
            },
        }
    if code != 0:
        return {
            "ok": False,
            "build_id": build_id,
            "build_dir": str(build),
            "stage": "build",
            "error": {
                "code": ErrorCode.BUILD_FAILED.value,
                "message": "cmake build failed",
                "details": {"exit_code": code, "output": output[-8000:]},
            },
        }
    return {
        "ok": True,
        "build_id": build_id,
        "build_dir": str(build),
        "qt_version": version["version"],
        "kit": kit["path"],
        "toolchain": toolchain,
        "build_type": build_type,
        "output": output[-4000:],
    }


@mcp.tool
async def qt_attach(
    descriptor: str | None = None,
    pid: int | None = None,
    session_id: str | None = None,
    mode: str = "auto",
    build_id: str | None = None,
) -> dict[str, Any]:
    """Attach to a live Qt process.

    Use this before any session-scoped tool. Use a rendezvous descriptor
    (linked mode), or ``pid`` with
    ``mode="injection"``. ``mode="auto"`` uses a live descriptor when
    available and otherwise injects. ``build_id`` guards against attaching a
    runtime built for a different library; a mismatch must be rebuilt/reinjected
    rather than bypassed.
    """
    if mode not in {"auto", "linked", "injection"}:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      f"unsupported attach mode: {mode}")
    if mode == "injection" and descriptor:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "descriptor cannot be combined with injection mode")
    if mode == "injection" and pid is None:
        return _error(ErrorCode.INVALID_ARGUMENT.value,
                      "injection mode requires pid")
    if build_id is not None:
        return _error(ErrorCode.NOT_IMPLEMENTED.value,
                      "build_id attach is not implemented")

    if mode == "injection":
        existing = _live_descriptors(pid)
        if existing:
            return _error(ErrorCode.MODE_CONFLICT.value,
                          "target already has a descriptor")
        injected = await _inject_target(pid)
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
                injected = await _inject_target(pid)
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
        session = Session(session_id, descriptor_pid, client, client.hello)
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


@mcp.tool
async def qt_detach(session_id: str, purge: bool = False) -> dict[str, Any]:
    """Close a session; ``purge=True`` requests descriptor removal.

    Detach before rebuilding/reinjecting a library if the old DLL is loaded.
    """
    if purge:
        return _error(ErrorCode.NOT_IMPLEMENTED.value,
                      "purge is not implemented")
    try:
        session = sessions.remove(session_id)
    except SessionNotFound:
        return _error(ErrorCode.SESSION_EXPIRED.value,
                      f"unknown session: {session_id}")
    await session.close()
    return {"ok": True, "session_id": session_id}


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
) -> dict[str, Any]:
    """Query the live object tree.

    Use when the target is unknown, or to survey a subtree. For one already
    known target, prefer qt_get. ``where`` accepts
    id/class/object_name/objectName/qml_id/qml_type/text/
    props; strings support exact, wildcard, and contains/startsWith/regex/
    equals matchers. ``scope`` limits to a window or subtree. Without
    ``props``, nodes include text/visible/enabled. ``limit`` is a soft
    truncation reported via count/total/limit/truncated.
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
        )
    except Exception as exc:
        return _map_error(exc)


@mcp.tool
async def qt_items(
    owner: str,
    where: dict[str, Any] | None = None,
    limit: int = 50,
    offset: int = 0,
    include_hidden: bool = False,
    compact: bool = False,
    session_id: str | None = None,
) -> dict[str, Any]:
    """List provider-backed children of a widget/QML owner.

    Use when the regular query tree lacks actions, tabs, cells, dock/toolbar
    items, or splitter handles. Covers actions, tabs, table/list/tree cells,
    dock/toolbar items, splitter
    handles, and QML item children. These children are virtual and are not
    part of the QWidget query tree.
    """
    session = _session(session_id or owner.split(".")[0])
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
) -> dict[str, Any]:
    """Run steps sequentially.

    Each step is exactly one of {"act": {...}}, {"wait": {...}},
    {"set": {...}}, or {"get": {...}}. Useful for write/read/restore
    sequences; ``stop_on_error`` is true by default.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.batch(session, steps, stop_on_error=stop_on_error)
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
) -> dict[str, Any]:
    """Write one property (destructive).

    Use for property setters such as
    QPushButton::setText, which are not qt_call methods. Hidden QML targets
    require ``include_hidden=true``.
    """
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    try:
        return await ops.set_property(session, target, name, value, nth=nth,
                                      include_hidden=include_hidden)
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
                                     include_hidden=include_hidden)
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
    Searches the target's source tree (and QT_COMMANDER_SOURCE_DIR) for a
    matching .ui/.qml; ``source_dir`` adds an explicit root for qrc/embedded
    QML. Returns file/line/url when found, otherwise E_NO_DEBUGINFO.
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


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()



