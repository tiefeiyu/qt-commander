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


def compose_build_id(
    *,
    qt_version: str | None,
    compiler: str | None,
    arch: str | None,
    build_type: str | None,
) -> str:
    """Return the canonical build id for an artifact or a build plan.

    ``qt_build`` returns this string, and ``manifest.json`` records it, so an
    agent can compare the two without knowing the older compiled-in format
    (``qt5/MSVC-AMD64/Release``) the runtime puts in its descriptor.
    """
    return (
        f"qt{qt_version or 'unknown'}/"
        f"{compiler or 'unknown'}-{arch or 'unknown'}/"
        f"{build_type or 'unknown'}"
    )


def manifest_build_id(manifest: dict[str, Any] | None) -> str | None:
    """Return a manifest's build id, deriving it for pre-2 manifests."""
    if not isinstance(manifest, dict):
        return None
    stored = manifest.get("build_id")
    if isinstance(stored, str) and stored:
        return stored
    compiler = (manifest.get("compiler") or {}).get("family")
    target = manifest.get("target") or {}
    qt = manifest.get("qt") or {}
    return compose_build_id(
        qt_version=qt.get("version"),
        compiler=compiler,
        arch=target.get("arch"),
        build_type=manifest.get("build_type"),
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
        "schema": 2,
        "build_id": compose_build_id(
            qt_version=qt_version,
            compiler=compiler,
            arch=arch,
            build_type=build_type,
        ),
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
    # An unknown target flavour (for example a PATH directory that ships both
    # Qt5Core.dll and Qt5Cored.dll) must not be silently treated as "release":
    # picking the wrong flavour injects a DLL that loads a second Qt runtime
    # and then times out waiting for QCoreApplication.
    if target.get("debug") is not None:
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


def listing(root: Path = ROOT) -> list[dict[str, Any]]:
    """Return every valid artifact manifest under the single root."""
    result: list[dict[str, Any]] = []
    if root.is_dir():
        for directory in sorted(root.glob("*/*/*")):
            manifest = read_manifest(directory)
            if manifest:
                result.append(
                    {"directory": str(directory), "manifest": manifest,
                     "build_id": manifest_build_id(manifest)}
                )
    return result


def find(target: dict[str, Any], root: Path = ROOT) -> dict[str, Any]:
    """Find exactly one artifact matching target metadata under one root.

    A target whose compiler version is unknown (common before injection) must
    not be blocked by an artifact built with an unknown compiler version: when
    several match, prefer the single artifact that pins a version, then the one
    that matches the target version exactly.
    """
    matches: list[tuple[Path, dict[str, Any]]] = []
    available: list[dict[str, Any]] = []
    if root.is_dir():
        for directory in root.glob("*/*/*"):
            if not directory.is_dir():
                continue
            manifest = read_manifest(directory)
            if not manifest:
                continue
            available.append(
                {"directory": str(directory), "manifest": manifest}
            )
            if _matches(manifest, target):
                matches.append((directory, manifest))

    def version_of(manifest: dict[str, Any]) -> str | None:
        return (manifest.get("compiler") or {}).get("version")

    if len(matches) > 1:
        family = (matches[0][1].get("compiler") or {}).get("family") or ""
        target_version = target.get("compiler_version")
        if target_version:
            exact = [
                item for item in matches
                if version_of(item[1])
                and compiler_abbrev(family, version_of(item[1]))
                == compiler_abbrev(family, target_version)
            ]
            if len(exact) == 1:
                matches = exact
        if len(matches) > 1:
            pinned = [item for item in matches if version_of(item[1])]
            if len(pinned) == 1:
                matches = pinned

    if not matches:
        expected = None
        try:
            expected = str(
                build_dir(
                    os_name=target.get("os", "unknown"),
                    arch=target.get("arch", "unknown"),
                    compiler=target.get("toolchain") or "unknown",
                    compiler_version=target.get("compiler_version"),
                    qt_version=target.get("qt_version"),
                    qt_debug=bool(target.get("debug")),
                    root=root,
                )
            )
        except Exception:
            pass
        return {
            "ok": False,
            "error": "E_ARTIFACT_NOT_FOUND",
            "message": "no artifact matches the target; run qt_build or provide "
                       "QT_COMMANDER_INJECT_DLL/QT_COMMANDER_INJECTOR",
            "searched": str(root),
            "expected": expected,
            "target": target,
            "available": available,
        }
    if len(matches) > 1:
        candidates = [
            {"directory": str(directory),
             "build_id": manifest_build_id(manifest),
             "build_type": manifest.get("build_type"),
             "compiler_version": version_of(manifest)}
            for directory, manifest in matches
        ]
        hint = ("retry qt_attach(pid=..., artifact_dir=<one of candidates>), "
                "or remove the stale duplicate artifact directory")
        if target.get("debug") is None:
            hint = ("target Qt debug/release is unknown; pass "
                    "artifact_dir=<one of candidates> explicitly (rebuild with "
                    "qt_build(for_pid=...) to pin the flavour)")
        return {
            "ok": False,
            "error": "E_ARTIFACT_AMBIGUOUS",
            "message": "multiple artifacts match the target",
            "searched": str(root),
            "candidates": candidates,
            "hint": hint,
            "available": available,
        }
    directory, manifest = matches[0]
    return {"ok": True, "directory": directory, "manifest": manifest}


def compiler_version(family: str, executable: str | None = None) -> str | None:
    """Return the compiler version, optionally from an explicit executable.

    ``qt_build`` resolves the compiler it is about to use (which may live
    outside PATH, e.g. a Qt kit toolchain); passing that executable here keeps
    the artifact directory name and the actual build in sync.
    """
    if not executable:
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
