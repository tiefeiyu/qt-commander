"""Thin adapter around the native qtc-inject executable.

Injection mechanics live in C++ (`src/injector`): the native injector runs on
the target platform, loads `qt_commander_inject.dll/.so`, and the target-side
library starts the same RPC server as linked mode.  Python only locates the
injector, invokes it, and maps its exit status into MCP errors.
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

from . import artifacts, target
from .errors import ErrorCode


class InjectionError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _artifact_for_pid(pid: int | None, artifact_dir: str | None = None) -> dict:
    if artifact_dir:
        directory = Path(artifact_dir)
        manifest = artifacts.read_manifest(directory)
        if manifest is None:
            return {
                "ok": False,
                "error": ErrorCode.ARTIFACT_NOT_FOUND.value,
                "message": f"no manifest.json under {directory}",
            }
        return {"ok": True, "directory": directory, "manifest": manifest}
    if pid is None:
        return {
            "ok": False,
            "error": ErrorCode.ARTIFACT_NOT_FOUND.value,
            "message": "pid is required to select an injector artifact",
        }
    try:
        info = target.target_info(pid=pid)
    except ValueError as exc:
        return {"ok": False, "error": ErrorCode.ARTIFACT_NOT_FOUND.value,
                "message": str(exc)}
    return artifacts.find(info)


def artifact_detail(pid: int | None, artifact_dir: str | None = None) -> dict:
    """Return the artifact lookup result, including why it failed."""
    return _artifact_for_pid(pid, artifact_dir)


def default_dll_path(pid: int | None = None,
                     artifact_dir: str | None = None) -> Path | None:
    override = os.environ.get("QT_COMMANDER_INJECT_DLL")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    found = _artifact_for_pid(pid, artifact_dir)
    if not found.get("ok"):
        return None
    relative = (found.get("manifest") or {}).get("library")
    if not relative:
        return None
    path = Path(found["directory"]) / relative
    return path if path.is_file() else None


def default_injector_path(pid: int | None = None,
                          artifact_dir: str | None = None) -> Path | None:
    override = os.environ.get("QT_COMMANDER_INJECTOR")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    found = _artifact_for_pid(pid, artifact_dir)
    if not found.get("ok"):
        return None
    relative = (found.get("manifest") or {}).get("injector")
    if not relative:
        return None
    path = Path(found["directory"]) / relative
    return path if path.is_file() else None


def inject_library(pid: int, lib_path: Path, *, timeout_ms: int = 8000,
                   artifact_dir: str | None = None,
                   injector_path: Path | None = None) -> None:
    # Resolve the injector once from the same artifact as the library; calling
    # default_injector_path() again would re-run artifact matching and can hit
    # the same ambiguity twice.
    injector = injector_path or default_injector_path(pid, artifact_dir)
    if injector is None:
        raise InjectionError(
            ErrorCode.INJECTION_FAILED.value,
            "native qtc-inject executable not found",
        )
    library = lib_path.resolve()
    if not library.is_file():
        raise InjectionError(
            ErrorCode.INJECTION_FAILED.value,
            f"injection library not found: {library}",
        )
    command = [str(injector), "--pid", str(pid), "--library", str(library)]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=max(8.0, timeout_ms / 1000.0 + 5.0),
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise InjectionError(
            ErrorCode.INJECTION_FAILED.value,
            f"qtc-inject failed: {exc}",
        ) from exc

    output = (completed.stdout or "").strip()
    if completed.returncode == 0:
        return
    message = output or (completed.stderr or "").strip() or "qtc-inject failed"
    code = ErrorCode.INJECTION_FAILED.value
    try:
        parsed = json.loads(output)
        message = str(parsed.get("message") or message)
        code = str(parsed.get("code") or code)
    except (json.JSONDecodeError, TypeError):
        pass
    if completed.returncode == 3:
        code = ErrorCode.PERMISSION.value
    elif completed.returncode == 2:
        code = ErrorCode.INVALID_ARGUMENT.value
    raise InjectionError(code, message)
