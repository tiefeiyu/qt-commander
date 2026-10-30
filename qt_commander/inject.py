"""Thin adapter around the native qtc-inject executable.

Injection mechanics live in C++ (`src/injector`): the native injector runs on
the target platform, loads `qt_commander_inject.dll/.so`, and the target-side
library starts the same RPC server as linked mode.  Python only locates the
injector, invokes it, and maps its exit status into MCP errors.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from .errors import ErrorCode


class InjectionError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _newest(paths: set[Path]) -> Path | None:
    if not paths:
        return None
    try:
        return max(paths, key=lambda path: path.stat().st_mtime)
    except OSError:
        return None


def default_dll_path() -> Path | None:
    override = os.environ.get("QT_COMMANDER_INJECT_DLL")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    root = Path(__file__).resolve().parents[1]
    candidates = {
        path.resolve()
        for pattern in (
            "build/*/qt-commander/qt_commander_inject.dll",
            "build/*/qt_commander_inject.dll",
            "build/*/qt-commander/libqt_commander_inject.so",
            "build/*/libqt_commander_inject.so",
        )
        for path in root.glob(pattern)
    }
    return _newest(candidates)


def default_injector_path() -> Path | None:
    override = os.environ.get("QT_COMMANDER_INJECTOR")
    if override:
        path = Path(override)
        return path if path.is_file() else None
    root = Path(__file__).resolve().parents[1]
    candidates = {
        path.resolve()
        for pattern in ("build/*/qt-commander/qtc-inject",
                        "build/*/qt-commander/qtc-inject.exe",
                        "build/*/qtc-inject",
                        "build/*/qtc-inject.exe")
        for path in root.glob(pattern)
    }
    return _newest(candidates)


def inject_library(pid: int, lib_path: Path, *, timeout_ms: int = 8000) -> None:
    injector = default_injector_path() or shutil.which("qtc-inject")
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
