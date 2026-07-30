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


BUILD_DIR = Path(".qt-commander/build")
INJECTOR_EXE_NAME = "qt-injector.exe" if os.name == "nt" else "qt-injector"
LIBRARY_NAME = "libqt-commander.dll" if os.name == "nt" else (
    "libqt-commander.dylib" if os.uname().sysname == "Darwin"
    else "libqt-commander.so"
)

_build_lock = asyncio.Lock()
_build_state = BuildState.NOT_BUILT


def detect_native_src() -> Path:
    """Detect the native C++ source directory.

    Priority:
    1. QT_COMMANDER_NATIVE_SRC env var
    2. <package>/native/ (pip install location)
    """
    env_src = os.environ.get("QT_COMMANDER_NATIVE_SRC")
    if env_src:
        p = Path(env_src)
        if p.exists():
            return p

    pkg_src = Path(__file__).parent / "native"
    if pkg_src.exists():
        return pkg_src

    cwd_src = Path.cwd() / "src"
    if cwd_src.exists():
        return cwd_src

    raise FileNotFoundError(
        "Cannot find C++ source. Set QT_COMMANDER_NATIVE_SRC or ensure native/ is installed."
    )


def _compute_source_hash(src_dir: Path) -> str:
    """Compute SHA-256 of all .cpp/.h files in src_dir."""
    hasher = hashlib.sha256()
    for pattern in ["**/*.cpp", "**/*.h", "**/*.hpp"]:
        for f in sorted(src_dir.glob(pattern)):
            hasher.update(f.read_bytes())
    return hasher.hexdigest()


def check_build_state(build_dir: Path | None = None) -> BuildState:
    """Check if injector and library are built and up-to-date."""
    global _build_state
    if build_dir is None:
        build_dir = BUILD_DIR

    injector_exe = build_dir / "injector" / "build" / INJECTOR_EXE_NAME
    library_file = build_dir / "library" / "build" / LIBRARY_NAME

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


async def run_build(
    vcvars_path: str,
    qt_env: str,
    vcvars_args: str = "",
    build_type: str = "Release",
    qt_major: int = 5,
    generator: str = "",
    build_dir: Path | None = None,
) -> dict:
    """Build injector and library using CMake."""
    global _build_state
    async with _build_lock:
        if _build_state == BuildState.BUILDING:
            raise QtCommanderError(2007, "build in progress")
        _build_state = BuildState.BUILDING

    try:
        if build_dir is None:
            build_dir = BUILD_DIR

        native_src = detect_native_src()

        vcvars_path = _sanitize_path_input(vcvars_path, "vcvars_path")
        qt_env = _sanitize_path_input(qt_env, "qt_env")
        vcvars_args = vcvars_args.strip()
        build_type = build_type.strip()
        generator = generator.strip()

        build_dir.mkdir(parents=True, exist_ok=True)
        injector_build = build_dir / "injector" / "build"
        library_build = build_dir / "library" / "build"

        _run_cmake_build(
            native_src / "src" / "injector",
            injector_build, build_type, generator,
            vcvars_path, vcvars_args, qt_env,
        )

        _run_cmake_build(
            native_src / "src" / "library",
            library_build, build_type, generator,
            vcvars_path, vcvars_args, qt_env,
            extra_args=[f"-DQT_MAJOR_VERSION={qt_major}", "-DBUILD_SERVER=OFF"],
        )

        manifest = {
            "source_hash": _compute_source_hash(native_src),
            "build_type": build_type,
            "qt_major": qt_major,
        }
        (build_dir / "build_manifest.json").write_text(json.dumps(manifest, indent=2))

        _build_state = BuildState.BUILT

        return {
            "injector_path": str(injector_build / INJECTOR_EXE_NAME),
            "library_path": str(library_build / LIBRARY_NAME),
            "qt_version": f"Qt{qt_major}",
            "arch": "x64",
        }
    except Exception:
        _build_state = BuildState.NOT_BUILT
        raise


def _run_cmake_build(
    src_dir: Path,
    build_dir: Path,
    build_type: str,
    generator: str,
    vcvars_path: str,
    vcvars_args: str,
    qt_env: str,
    extra_args: list[str] | None = None,
) -> None:
    """Run cmake configure + build for a single target."""
    build_dir.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        "cmake", "-S", str(src_dir), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=" + build_type,
    ]
    if generator:
        cmake_args.extend(["-G", generator])
    if extra_args:
        cmake_args.extend(extra_args)

    build_cmd = ["cmake", "--build", str(build_dir), "--config", build_type]

    if os.name == "nt":
        script = (
            f'@echo off\r\n'
            f'call "{vcvars_path}" {vcvars_args}\r\n'
            f'call "{qt_env}"\r\n'
            f'{" ".join(cmake_args)}\r\n'
            f'{" ".join(build_cmd)}\r\n'
        )
        result = subprocess.run(
            ["cmd.exe", "/c", script],
            capture_output=True, text=True, timeout=600,
        )
    else:
        script = (
            f'source "{qt_env}" && '
            f'{" ".join(cmake_args)} && '
            f'{" ".join(build_cmd)}'
        )
        result = subprocess.run(
            ["bash", "-c", script],
            capture_output=True, text=True, timeout=600,
        )

    if result.returncode != 0:
        raise QtCommanderError(
            2001,
            f"Build failed for {src_dir.name}:\n"
            f"{result.stderr[-500:] or result.stdout[-500:]}"
        )
