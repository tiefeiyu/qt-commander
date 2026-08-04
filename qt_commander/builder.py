"""Build orchestration: detect source, check state, run CMake builds."""
import asyncio
import hashlib
import json
import os
import subprocess
from enum import Enum
from pathlib import Path

from .errors import QtCommanderError


class BuildState(Enum):
    NOT_BUILT = "not_built"
    BUILDING = "building"
    BUILT = "built"


BUILD_DIR = Path(".qt-commander")

INJECTOR_EXE_NAME = "qt-injector.exe" if os.name == "nt" else "qt-injector"
LIBRARY_NAME = "libqt-commander.dll" if os.name == "nt" else (
    "libqt-commander.dylib" if os.uname().sysname == "Darwin"
    else "libqt-commander.so"
)

_build_lock = asyncio.Lock()
_build_state = BuildState.NOT_BUILT


def detect_native_src() -> Path:
    """Detect the project root (parent of the native C++ ``src/`` directory).

    Priority:
    1. ``QT_COMMANDER_NATIVE_SRC`` env var — if set, treated as project root
    2. ``<package>/native/`` (pip install location)
    3. Current working directory (must contain ``src/``)
    """
    env_src = os.environ.get("QT_COMMANDER_NATIVE_SRC")
    if env_src:
        p = Path(env_src)
        if p.exists():
            return p

    pkg_src = Path(__file__).parent / "native"
    if pkg_src.exists():
        return pkg_src

    cwd = Path.cwd()
    if (cwd / "src").exists():
        return cwd

    raise FileNotFoundError(
        "Cannot find C++ source. Set QT_COMMANDER_NATIVE_SRC "
        "or ensure src/ exists in the working directory."
    )


def _compute_source_hash(src_dir: Path) -> str:
    """Compute SHA-256 of all .cpp/.h files in src_dir.

    When src_dir is the project root, only the native sources under
    ``src/`` count: globbing the whole root would also hash generated
    build files (e.g. .qt-commander/build/.../mocs_compilation_*.cpp)
    that change on every build, making the hash never match the manifest.
    """
    native_root = src_dir / "src"
    if not native_root.is_dir():
        native_root = src_dir  # caller passed the src dir directly
    hasher = hashlib.sha256()
    for pattern in ["**/*.cpp", "**/*.h", "**/*.hpp"]:
        for f in sorted(native_root.glob(pattern)):
            hasher.update(f.read_bytes())
    return hasher.hexdigest()


def _out_dir(build_dir: Path | None = None) -> Path:
    """The workspace root where cmake --install places artifacts (into bin/)."""
    return build_dir or BUILD_DIR


def check_build_state(build_dir: Path | None = None) -> BuildState:
    """Check if injector and library are built and up-to-date.

    Looks for installed artifacts under ``<build_dir>/install/bin/``,
    which is the final output of ``cmake --install``.
    """
    global _build_state
    if build_dir is None:
        build_dir = BUILD_DIR

    idir = _out_dir(build_dir)
    injector_exe = idir / "bin" / INJECTOR_EXE_NAME
    library_file = idir / "bin" / LIBRARY_NAME

    if not injector_exe.exists() or injector_exe.stat().st_size == 0:
        _build_state = BuildState.NOT_BUILT
        return BuildState.NOT_BUILT
    if not library_file.exists() or library_file.stat().st_size == 0:
        _build_state = BuildState.NOT_BUILT
        return BuildState.NOT_BUILT

    manifest_file = build_dir / "build_manifest.json"
    if manifest_file.exists():
        try:
            manifest = json.loads(manifest_file.read_text())
            try:
                native_src = detect_native_src()
                current_hash = _compute_source_hash(native_src)
                if manifest.get("source_hash") != current_hash:
                    _build_state = BuildState.NOT_BUILT
                    return BuildState.NOT_BUILT
            except FileNotFoundError:
                pass
        except (json.JSONDecodeError, KeyError):
            pass

    _build_state = BuildState.BUILT
    return BuildState.BUILT


def _sanitize_path_input(value: str, name: str) -> str:
    """Reject inputs with control characters, quotes, or newlines."""
    for ch in value:
        if ord(ch) < 0x20 and ch != "\t":
            raise ValueError(f"Invalid character (0x{ord(ch):02x}) in {name}")
        if ch in ('"', "'", "`"):
            raise ValueError(f"Quote character not allowed in {name}")
    return value


def _find_build_script() -> Path:
    """Find the Windows build batch script."""
    # 1. next to the package
    pkg_scripts = Path(__file__).resolve().parent.parent / "scripts"
    bat = pkg_scripts / "build_windows.bat"
    if bat.exists():
        return bat
    # 2. in the project root (detected from CWD)
    cwd_bat = Path.cwd() / "scripts" / "build_windows.bat"
    if cwd_bat.exists():
        return cwd_bat
    raise FileNotFoundError(
        "Cannot find scripts/build_windows.bat. "
        "Ensure the project is installed correctly."
    )


_TOOLCHAINS = ("msvc", "mingw")


async def run_build(
    vcvars_path: str,
    qt_env: str,
    vcvars_args: str = "",
    build_type: str = "Release",
    qt_major: int = 5,
    generator: str = "",
    with_qml: bool = True,
    toolchain: str = "msvc",
    build_dir: Path | None = None,
) -> dict:
    """Build injector and library by calling the Windows build script.

    ``toolchain`` selects how the script sets up the compiler:
      "msvc"  — ``vcvars_path`` is a vcvars bat, ``qt_env`` a qtenv2.bat
      "mingw" — ``vcvars_path`` is the MinGW bin dir; ``qt_env`` the kit's
                qtenv2.bat (MinGW Qt kits ship one too)
    """
    global _build_state
    async with _build_lock:
        if _build_state == BuildState.BUILDING:
            raise QtCommanderError(2007, "build in progress")
        _build_state = BuildState.BUILDING

    try:
        if build_dir is None:
            build_dir = BUILD_DIR

        native_src = detect_native_src()
        build_script = _find_build_script()

        toolchain = toolchain.strip().lower()
        if toolchain not in _TOOLCHAINS:
            raise ValueError(
                f"Invalid toolchain '{toolchain}' — expected one of "
                f"{', '.join(_TOOLCHAINS)}")

        vcvars_path = _sanitize_path_input(vcvars_path, "vcvars_path")
        qt_env = _sanitize_path_input(qt_env, "qt_env")
        vcvars_args = vcvars_args.strip()
        build_type = build_type.strip()
        generator = generator.strip()

        build_dir.mkdir(parents=True, exist_ok=True)
        cmake_build_dir = build_dir / "build"
        install_prefix = build_dir  # cmake installs to bin/ within the prefix

        # Call the batch script: all absolute paths to avoid CWD issues
        cmd = [
            str(build_script.resolve()),
            vcvars_path,
            vcvars_args,
            qt_env,
            str(native_src.resolve()),
            str(cmake_build_dir.resolve()),
            str(install_prefix.resolve()),
            build_type,
            str(qt_major),
            "ON" if with_qml else "OFF",
        ]
        # Always append the generator slot (empty string included): the bat
        # reads positional args %10 (generator) and %11 (toolchain) after
        # shifting, so dropping the empty generator would misalign
        # toolchain into the generator slot and silently fall back to msvc.
        cmd.append(generator)
        cmd.append(toolchain)

        result = subprocess.run(
            ["cmd.exe", "/c"] + cmd,
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=600,
            cwd=str(native_src.resolve()),
        )

        if result.returncode != 0:
            raise QtCommanderError(
                2001,
                f"Build failed:\n{result.stderr[-500:] or result.stdout[-500:]}"
            )

        manifest = {
            "source_hash": _compute_source_hash(native_src),
            "build_type": build_type,
            "qt_major": qt_major,
        }
        (build_dir / "build_manifest.json").write_text(json.dumps(manifest, indent=2))

        _build_state = BuildState.BUILT

        return {
            "injector_path": str(install_prefix / "bin" / INJECTOR_EXE_NAME),
            "library_path": str(install_prefix / "bin" / LIBRARY_NAME),
            "qt_version": f"Qt{qt_major}",
            "arch": "x64",
        }
    except Exception:
        _build_state = BuildState.NOT_BUILT
        raise
