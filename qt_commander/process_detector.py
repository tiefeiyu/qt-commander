"""Detect running Qt processes across platforms."""
import sys
from pathlib import Path

import psutil


def list_qt_processes() -> list[dict]:
    """Return list of running Qt processes with metadata."""
    results = []
    for proc in psutil.process_iter(["pid", "name", "exe"]):
        try:
            pid = proc.info["pid"]
            if pid is None:
                continue

            exe_path = proc.info.get("exe") or ""
            is_qt = False
            qt_version = ""
            arch = ""
            bitness = 64 if sys.maxsize > 2 ** 32 else 32

            if sys.platform == "win32":
                is_qt, qt_version, arch, bitness = _check_qt_windows(proc)
            elif sys.platform == "linux":
                is_qt, qt_version, arch = _check_qt_linux(pid)
            elif sys.platform == "darwin":
                is_qt, qt_version = _check_qt_macos(proc)

            if not is_qt:
                continue

            title = ""
            try:
                if proc.name():
                    title = proc.name()
            except Exception:
                pass

            results.append({
                "pid": pid,
                "name": proc.info.get("name", ""),
                "title": title,
                "qt_version": qt_version,
                "arch": arch,
                "bitness": bitness,
                "exe_path": exe_path,
            })
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return results


def _check_qt_windows(proc: psutil.Process) -> tuple:
    """Check if a Windows process has Qt DLLs loaded."""
    try:
        mmaps = proc.memory_maps()
    except psutil.AccessDenied:
        return False, "", "", 0

    qt_dlls = {"Qt5Core": "5", "Qt6Core": "6"}
    for mm in mmaps:
        path_lower = Path(mm.path).name.lower()
        for qt_name, version in qt_dlls.items():
            if qt_name.lower() in path_lower:
                arch = "x64" if "64" in (proc.name() or "") or (proc.exe() and "64" in proc.exe()) else "x86"
                bitness = 64 if arch == "x64" else 32
                return True, version, arch, bitness
    return False, "", "", 0


def _check_qt_linux(pid: int) -> tuple:
    """Check /proc/<pid>/maps for libQt*Core.so."""
    try:
        with open(f"/proc/{pid}/maps", "r") as f:
            for line in f:
                if "libQt5Core.so" in line:
                    return True, "5", "x86_64"
                if "libQt6Core.so" in line:
                    return True, "6", "x86_64"
    except (PermissionError, FileNotFoundError):
        pass
    return False, "", ""


def _check_qt_macos(proc: psutil.Process) -> tuple:
    """Check for QtCore.framework in process memory maps."""
    try:
        mmaps = proc.memory_maps()
        for mm in mmaps:
            if "QtCore.framework" in mm.path:
                return True, "5"
    except psutil.AccessDenied:
        pass
    return False, ""
