"""One-root artifact layout for injectors and injection libraries.

Layout:
    artifacts/<os>-<arch>/<compiler>-<major.minor>/qt-<version>-<debug|release>/
        manifest.json
        bin/qtc-inject[.exe]
        lib/qt_commander_inject.dll | libqt_commander_inject.so
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1] / "artifacts"


def default_root() -> Path:
    """Artifact root used by every lookup.

    ``QT_COMMANDER_ARTIFACTS`` overrides the in-checkout ``artifacts/``
    directory. An installed wheel cannot write inside ``site-packages``, so
    ``qt-commander install-artifacts`` points this at a per-user directory.
    """
    override = os.environ.get("QT_COMMANDER_ARTIFACTS")
    if override:
        return Path(override).expanduser()
    return ROOT


def os_slug() -> str:
    system = platform.system().lower()
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    if system == "darwin":
        return "macos"
    return system or "unknown"


def host_arch() -> str:
    """Map ``platform.machine()`` onto the artifact arch slug (x64/x86/arm64)."""
    machine = (platform.machine() or "").lower()
    if machine in {"amd64", "x86_64", "x64", "em64t"}:
        return "x64"
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86", "i386", "i486", "i586", "i686"}:
        return "x86"
    return machine or "unknown"


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
    root: Path | None = None,
) -> Path:
    base = Path(root) if root is not None else default_root()
    return (
        base
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
    source: dict[str, Any] | None = None,
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
    if source:
        manifest["source"] = dict(source)
    path = directory / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path


def read_manifest(directory: Path) -> dict[str, Any] | None:
    try:
        data = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


BUILD_INFO_FILE = "qtc-build-info.json"


def read_build_info(build: Path) -> dict[str, Any]:
    """Read the version/commit CMake recorded next to a build tree.

    This is the authoritative statement of what was compiled: the same tag that
    produced the package version produced ``QT_COMMANDER_VERSION``, and the
    commit identifies the checkout when the version alone cannot (dev builds).
    """
    try:
        data = json.loads((build / BUILD_INFO_FILE).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    return {
        key: data[key]
        for key in ("version", "commit", "describe")
        if isinstance(data.get(key), str) and data[key]
    }


_BUILT_INJECTORS = ("qtc-inject.exe", "qtc-inject")
_BUILT_LIBRARIES = ("qt_commander_inject.dll", "libqt_commander_inject.so")


def find_built_file(build: Path, names: tuple[str, ...]) -> Path | None:
    """Find a CMake output without picking up an already-installed copy."""
    for name in names:
        matches = sorted(
            path for path in build.rglob(name)
            if (build / "bin") not in path.parents
            and (build / "lib") not in path.parents
        )
        if matches:
            return matches[0]
    return None


def install_from_build(
    build: Path,
    *,
    os_name: str,
    arch: str,
    compiler: str,
    compiler_version: str | None,
    qt_version: str | None,
    qt_debug: bool,
    build_type: str,
    root: Path | None = None,
) -> dict[str, Any]:
    """Copy a CMake build tree into the artifact layout and write its manifest.

    Shared by ``qt_build`` and the release packaging script so a hand-built
    runtime and an agent-built one always land in the same place with the same
    manifest shape.
    """
    artifact_dir = build_dir(
        os_name=os_name,
        arch=arch,
        compiler=compiler,
        compiler_version=compiler_version,
        qt_version=qt_version,
        qt_debug=qt_debug,
        root=root,
    )
    injector = find_built_file(build, _BUILT_INJECTORS)
    library = find_built_file(build, _BUILT_LIBRARIES)
    source = read_build_info(build) or None
    manifest = None
    injector_path = None
    library_path = None
    if injector and library:
        bin_dir = artifact_dir / "bin"
        lib_dir = artifact_dir / "lib"
        bin_dir.mkdir(parents=True, exist_ok=True)
        lib_dir.mkdir(parents=True, exist_ok=True)
        injector_path = bin_dir / injector.name
        library_path = lib_dir / library.name
        shutil.copy2(injector, injector_path)
        shutil.copy2(library, library_path)
        manifest = write_manifest(
            artifact_dir,
            os_name=os_name,
            arch=arch,
            compiler=compiler,
            compiler_version=compiler_version,
            qt_version=qt_version,
            qt_debug=qt_debug,
            build_type=build_type,
            injector=f"bin/{injector.name}",
            library=f"lib/{library.name}",
            source=source,
        )
    return {
        "artifact_dir": artifact_dir,
        "injector": injector_path,
        "library": library_path,
        "manifest": manifest,
        "build_id": compose_build_id(
            qt_version=qt_version,
            compiler=compiler,
            arch=arch,
            build_type=build_type,
        ),
        "source": source,
    }


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


def listing(root: Path | None = None) -> list[dict[str, Any]]:
    """Return every valid artifact manifest under the single root."""
    base = Path(root) if root is not None else default_root()
    result: list[dict[str, Any]] = []
    if base.is_dir():
        for directory in sorted(base.glob("*/*/*")):
            manifest = read_manifest(directory)
            if manifest:
                source = manifest.get("source") or {}
                result.append(
                    {"directory": str(directory), "manifest": manifest,
                     "build_id": manifest_build_id(manifest),
                     "version": source.get("version"),
                     "commit": source.get("commit")}
                )
    return result


def find(target: dict[str, Any], root: Path | None = None) -> dict[str, Any]:
    """Find exactly one artifact matching target metadata under one root.

    A target whose compiler version is unknown (common before injection) must
    not be blocked by an artifact built with an unknown compiler version: when
    several match, prefer the single artifact that pins a version, then the one
    that matches the target version exactly.
    """
    base = Path(root) if root is not None else default_root()
    matches: list[tuple[Path, dict[str, Any]]] = []
    available: list[dict[str, Any]] = []
    if base.is_dir():
        for directory in base.glob("*/*/*"):
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
                    root=base,
                )
            )
        except Exception:
            pass
        return {
            "ok": False,
            "error": "E_ARTIFACT_NOT_FOUND",
            "message": "no artifact matches the target; run qt_build or provide "
                       "QT_COMMANDER_INJECT_DLL/QT_COMMANDER_INJECTOR",
            "searched": str(base),
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
            "searched": str(base),
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

    MSVC's ``cl`` has no ``--version`` flag: it always prints its banner, so it
    is invoked with no arguments and the banner is parsed instead.
    """
    if not executable:
        executable = {
            "mingw": shutil.which("g++") or shutil.which("gcc"),
            "gcc": shutil.which("gcc"),
            "clang": shutil.which("clang++"),
            "msvc": shutil.which("cl"),
        }.get(family)
    if not executable:
        return None
    command = [executable] if family == "msvc" else [executable, "--version"]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    # Look at both streams: MSVC writes its usage to stdout and the version
    # banner to stderr, and the banner is localized (the console code page can
    # contain bytes that are invalid UTF-8, which used to abort text-mode
    # capture and silently lose the version).
    raw = (completed.stdout or b"") + b"\n" + (completed.stderr or b"")
    text = raw.decode("utf-8", errors="replace")
    for line in text.splitlines():
        tokens = line.replace("(", " ").replace(")", " ").split()
        for token in tokens:
            if token and token[0].isdigit() and "." in token:
                return token.rstrip(",")
    return None
