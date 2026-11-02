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

from . import inject, ops
from .client import ClientClosed, ClientTimeout, RpcClient, RpcError
from .environment import detect_qt_environments, detect_toolchains
from .errors import ErrorCode
from .session import Session, SessionExists, SessionManager, SessionNotFound

mcp = FastMCP("qt-commander")
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
    """Return live sessions/descriptors, optionally probing one session."""
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
async def qt_windows(
    session_id: str,
    include_hidden: bool = False,
    visible: bool | None = None,
    class_name: str | None = None,
    limit: int = 200,
    offset: int = 0,
) -> dict[str, Any]:
    """List top-level windows with id/title/class/screen_pos/active/modal."""
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
    """Detect Qt versions/kits and C++ toolchains available on this machine."""
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
    """Configure and build a CMake project against a discovered Qt kit."""
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
    """Attach to a linked runtime via its descriptor."""
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
    """Close a session; purge is reserved for descriptor removal."""
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
    """Live query of the object tree ."""
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
    """List non-QObject children of an owner (provider chain)."""
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
    """Dispatch one action (click/type/focus today; more in the engine)."""
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
    """Run act/wait steps sequentially ."""
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
    """Poll a declarative condition ."""
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
    format: str = "png",
) -> list[Any]:
    """Capture a PNG plus metadata.

    With no target, exactly one visible top-level window is captured; if
    several are visible the call fails with ``E_AMBIGUOUS`` and the caller
    must choose a window from ``qt_windows``.

    With ``annotate`` enabled, ``legend`` lists the numbered regions
    (``n``, ``id``, ``rect``, ``image_rect``). The number is only a
    screenshot-local visual marker; use ``id`` from the legend to act.
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
    """Read one node/virtual node ."""
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
    """Set a property (destructive)."""
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
    """Invoke a method (destructive)."""
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
) -> dict[str, Any]:
    """List invokable Qt methods (slots/Q_INVOKABLE) for a target."""
    session = _session(session_id)
    if isinstance(session, dict):
        return session
    params: dict[str, Any] = {"target": target}
    if nth is not None:
        params["nth"] = nth
    if include_hidden:
        params["include_hidden"] = True
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
    """Return source location (engine support pending)."""
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



