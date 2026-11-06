"""One-root artifact layout for injectors and injection libraries.

Layout:
    artifacts/<os>-<arch>/<compiler>-<major.minor>/qt-<version>-<debug|release>/
        manifest.json
        bin/qtc-inject[.exe]
        lib/qt_commander_inject.dll | libqt_commander_inject.so
"""

from __future__ import annotations

import json
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1] / "artifacts"


def os_slug() -> str:
    system = platform.system().lower()
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    if system == "darwin":
        return "macos"
    return system or "unknown"


def short_version(version: str | None, parts: int = 2) -> str:
    if not version:
        return "unknown"
    values = []
    for part in str(version).replace("-", ".").split("."):
        digits = "".join(ch for ch in part if ch.isdigit())
        if digits:
            values.append(digits)
        if len(values) >= parts:
            break
    return ".".join(values) or "unknown"


def compiler_abbrev(family: str, version: str | None) -> str:
    """Normalize compiler versions used in paths/manifests.

    MSVC's toolset directory is 14.x while cl reports 19.x; normalize both to
    the 19.x form so a target descriptor and a build manifest compare equal.
    """
    suffix = short_version(version)
    if (family or "").lower() == "msvc":
        parts = suffix.split(".")
        if parts and parts[0] == "14":
            parts[0] = "19"
        return ".".join(parts)
    return suffix


def compiler_slug(family: str, version: str | None) -> str:
    family = (family or "unknown").lower()
    suffix = compiler_abbrev(family, version)
    if family == "msvc":
        return f"msvc-{suffix}"
    if family == "mingw":
        return f"mingw-gcc-{suffix}"
    if family == "gcc":
        return f"gcc-{suffix}"
    if family == "clang":
        return f"clang-{suffix}"
    return f"{family}-{suffix}"


def qt_slug(version: str | None, debug: bool) -> str:
    return f"qt-{version or 'unknown'}-{'debug' if debug else 'release'}"


def build_dir(
    *,
    os_name: str,
    arch: str,
    compiler: str,
    compiler_version: str | None,
    qt_version: str | None,
    qt_debug: bool,
    root: Path = ROOT,
) -> Path:
    return (
        root
        / f"{os_name}-{arch}"
        / compiler_slug(compiler, compiler_version)
        / qt_slug(qt_version, qt_debug)
    )


def write_manifest(
    directory: Path,
    *,
    os_name: str,
    arch: str,
    compiler: str,
    compiler_version: str | None,
    qt_version: str | None,
    qt_debug: bool,
    build_type: str,
    injector: str,
    library: str,
) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": 1,
        "target": {"os": os_name, "arch": arch},
        "compiler": {
            "family": compiler,
            "version": compiler_version,
        },
        "qt": {"version": qt_version, "debug": qt_debug},
        "build_type": build_type,
        "injector": injector,
        "library": library,
    }
    path = directory / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path


def read_manifest(directory: Path) -> dict[str, Any] | None:
    try:
        data = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def _matches(manifest: dict[str, Any], target: dict[str, Any]) -> bool:
    m_target = manifest.get("target") or {}
    m_compiler = manifest.get("compiler") or {}
    m_qt = manifest.get("qt") or {}
    if m_target.get("os") != target.get("os"):
        return False
    if m_target.get("arch") != target.get("arch"):
        return False
    if m_qt.get("version") != target.get("qt_version"):
        return False
    if bool(m_qt.get("debug")) != bool(target.get("debug")):
        return False
    if m_compiler.get("family") != target.get("toolchain"):
        return False
    target_compiler = target.get("compiler_version")
    if target_compiler and m_compiler.get("version"):
        family = m_compiler.get("family") or ""
        if compiler_abbrev(family, m_compiler["version"]) != compiler_abbrev(
            family, target_compiler
        ):
            return False
    return True


def find(target: dict[str, Any], root: Path = ROOT) -> dict[str, Any]:
    """Find exactly one artifact matching target metadata under one root."""
    matches: list[tuple[Path, dict[str, Any]]] = []
    if root.is_dir():
        for directory in root.glob("*/*/*"):
            if not directory.is_dir():
                continue
            manifest = read_manifest(directory)
            if manifest and _matches(manifest, target):
                matches.append((directory, manifest))
    if not matches:
        return {
            "ok": False,
            "error": "E_ARTIFACT_NOT_FOUND",
            "message": "no artifact matches the target; run qt_build or provide "
                       "QT_COMMANDER_INJECT_DLL/QT_COMMANDER_INJECTOR",
            "searched": str(root),
        }
    if len(matches) > 1:
        return {
            "ok": False,
            "error": "E_ARTIFACT_AMBIGUOUS",
            "message": "multiple artifacts match the target; specify the "
                       "artifact directory explicitly",
            "candidates": [str(directory) for directory, _ in matches],
        }
    directory, manifest = matches[0]
    return {"ok": True, "directory": directory, "manifest": manifest}


def compiler_version(family: str) -> str | None:
    executable = {
        "mingw": shutil.which("g++") or shutil.which("gcc"),
        "gcc": shutil.which("gcc"),
        "clang": shutil.which("clang++"),
    }.get(family)
    if not executable:
        return None
    try:
        completed = subprocess.run(
            [executable, "--version"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    first = (completed.stdout or completed.stderr).splitlines()
    if not first:
        return None
    tokens = first[0].replace("(", " ").replace(")", " ").split()
    for token in tokens:
        if token and token[0].isdigit() and "." in token:
            return token
    return None
