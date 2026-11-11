"""Lightweight Qt/toolchain discovery for the MCP environment tool.

This intentionally does not scan the whole disk: it checks explicit roots,
well-known install locations, and PATH.  The old build/injection stack is gone;
this module only reports what is available.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Iterable, Sequence

_VERSION_RE = re.compile(r"^\d+\.\d+(?:\.\d+)?$")


def _default_roots() -> list[Path]:
    roots: list[Path] = []
    for name in ("QTDIR", "QT_DIR", "Qt5_DIR", "Qt6_DIR"):
        value = os.environ.get(name)
        if value:
            path = Path(value)
            # Qt5_DIR points at lib/cmake/Qt5; walk up to the kit root.
            roots.append(path)
            for parent in list(path.parents)[:4]:
                roots.append(parent)
    for name in ("qmake", "qtpaths"):
        found = shutil.which(name)
        if found:
            roots.append(Path(found).resolve().parent.parent)
    roots.extend(
        [
            Path("C:/Qt"),
            Path("C:/Software/Qt"),
            Path.home() / "Qt",
            Path("/opt/Qt"),
            Path("/usr/local/Qt"),
        ]
    )
    return roots


def _kit_compiler(name: str) -> str:
    lowered = name.lower()
    if lowered.startswith("msvc"):
        return "msvc"
    if lowered.startswith("mingw"):
        return "mingw"
    if lowered.startswith("gcc"):
        return "gcc"
    if lowered.startswith("clang"):
        return "clang"
    return "unknown"


def _kit_is_usable(kit: Path) -> bool:
    return (
        (kit / "bin" / "qmake.exe").is_file()
        or (kit / "bin" / "qmake").is_file()
        or (kit / "lib" / "cmake" / "Qt5" / "Qt5Config.cmake").is_file()
        or (kit / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake").is_file()
    )


def detect_qt_environments(
    roots: Sequence[str | Path] | None = None,
) -> list[dict]:
    """Return Qt versions and kits found under the candidate roots."""
    candidates = [Path(root) for root in roots] if roots is not None else _default_roots()
    seen_roots: set[str] = set()
    found: dict[str, dict] = {}

    for root in candidates:
        try:
            resolved = root.resolve()
        except OSError:
            continue
        key = str(resolved).lower()
        if key in seen_roots or not resolved.is_dir():
            continue
        seen_roots.add(key)

        for version_dir in sorted(resolved.iterdir()):
            if not version_dir.is_dir() or not _VERSION_RE.fullmatch(version_dir.name):
                continue
            kits: list[dict] = []
            for kit in sorted(version_dir.iterdir()):
                if not kit.is_dir() or not _kit_is_usable(kit):
                    continue
                kits.append(
                    {
                        "name": kit.name,
                        "path": str(kit),
                        "compiler": _kit_compiler(kit.name),
                        "cmake_dir": str(kit / "lib" / "cmake"),
                    }
                )
            if kits:
                found.setdefault(
                    version_dir.name,
                    {
                        "version": version_dir.name,
                        "major": int(version_dir.name.split(".", 1)[0]),
                        "root": str(version_dir),
                        "kits": kits,
                    },
                )
    return sorted(found.values(), key=lambda item: item["version"], reverse=True)


def _toolchain(path: str, name: str) -> dict:
    version = None
    try:
        completed = subprocess.run(
            [path, "--version"],
            capture_output=True,
            timeout=2,
            check=False,
        )
        # Both streams, decoded leniently: localized banners can contain bytes
        # that are not valid UTF-8, and some compilers split usage/version
        # across stdout and stderr.
        raw = (completed.stdout or b"") + b"\n" + (completed.stderr or b"")
        lines = [line.strip() for line
                 in raw.decode("utf-8", errors="replace").splitlines()
                 if line.strip()]
        for line in lines:
            if any(ch.isdigit() for ch in line) and "." in line:
                version = line
                break
        else:
            version = lines[0] if lines else None
    except (OSError, subprocess.SubprocessError):
        pass
    return {"name": name, "path": path, "version": version}


def _vswhere_path() -> Path | None:
    candidates = [
        Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
        Path(os.environ.get("ProgramFiles", "C:/Program Files"))
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _visual_studio_toolchains() -> list[dict]:
    if os.name != "nt":
        return []
    vswhere = _vswhere_path()
    if vswhere is None:
        return []
    try:
        completed = subprocess.run(
            [str(vswhere), "-all", "-products", "*", "-format", "json"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        installations = json.loads(completed.stdout or "[]")
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return []
    result: list[dict] = []
    for installation in installations:
        root = Path(installation.get("installationPath", ""))
        if not root.is_dir():
            continue
        tools = root / "VC" / "Tools" / "MSVC"
        compilers = sorted(tools.glob("*/bin/Hostx64/x64/cl.exe"),
                           reverse=True)
        if not compilers:
            continue
        vcvars = root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        result.append({
            "name": "msvc",
            "path": str(compilers[0]),
            "version": compilers[0].parent.parent.parent.parent.name,
            "vcvars": str(vcvars) if vcvars.is_file() else None,
        })
    return result


def detect_toolchains() -> list[dict]:
    """Report C++ compilers on PATH and Visual Studio via vswhere."""
    result: list[dict] = []
    for name in ("g++", "clang++", "cl"):
        found = shutil.which(name)
        if found:
            result.append(_toolchain(found, name))
    for toolchain in _visual_studio_toolchains():
        if not any(item["path"] == toolchain["path"] for item in result):
            result.append(toolchain)
    return result
