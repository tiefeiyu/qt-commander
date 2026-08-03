"""Detect running Qt processes across platforms."""
import struct
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
                is_qt, qt_version, arch, bitness = _check_qt_windows(proc, exe_path)
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


def _detect_arch(exe_path: str) -> tuple[str, int]:
    """Read IMAGE_FILE_HEADER.Machine from the exe's PE header.

    Returns ("", 0) when the header cannot be read (no path, protected
    process, missing/corrupt file).
    """
    if not exe_path:
        return "", 0
    try:
        with open(exe_path, "rb") as f:
            dos_header = f.read(64)
            if len(dos_header) < 64 or dos_header[:2] != b"MZ":
                return "", 0
            pe_offset = struct.unpack_from("<I", dos_header, 0x3C)[0]
            f.seek(pe_offset + 4)
            machine_bytes = f.read(2)
            if len(machine_bytes) != 2:
                return "", 0
            machine = struct.unpack_from("<H", machine_bytes)[0]
    except OSError:
        return "", 0
    if machine == 0x8664:  # AMD64
        return "x64", 64
    if machine == 0xAA64:  # ARM64
        return "arm64", 64
    return "x86", 32  # 0x14C (i386) and other legacy types


def _check_qt_windows(proc: psutil.Process, exe_path: str) -> tuple:
    """Check if a Windows process has Qt DLLs loaded."""
    try:
        mmaps = proc.memory_maps()
    except psutil.AccessDenied:
        return False, "", "", 0

    arch, bitness = _detect_arch(exe_path)

    qt_dlls = {"Qt5Core": "5", "Qt6Core": "6"}
    for mm in mmaps:
        path_lower = Path(mm.path).name.lower()
        for qt_name, version in qt_dlls.items():
            if qt_name.lower() in path_lower:
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
