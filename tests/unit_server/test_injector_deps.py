"""Verify the injector's dependency-closure resolution via --list-deps.

The injector preloads the transitive dependency closure of
libqt-commander.dll into the target process before injecting the library
itself (the Windows loader does not search the parent DLL's directory for
ITS dependencies, so the whole closure must be preloaded).  This test
pins the closure against the real built library.
"""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
INJECTOR = REPO / ".qt-commander" / "bin" / "qt-injector.exe"
LIBRARY = REPO / ".qt-commander" / "bin" / "libqt-commander.dll"


def _run_list_deps(dll: Path, search_dirs: list[Path]) -> list[str]:
    args = [str(INJECTOR), "--list-deps", str(dll)]
    for d in search_dirs:
        args += ["--search-dir", str(d)]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, f"injector failed: {proc.stderr}"
    return json.loads(proc.stdout)["deps"]


@pytest.fixture(scope="module")
def injector_and_library() -> tuple[Path, Path]:
    if not INJECTOR.exists():
        pytest.skip("qt-injector.exe not built (run qt_build first)")
    if not LIBRARY.exists():
        pytest.skip("libqt-commander.dll not built (run qt_build first)")
    return INJECTOR, LIBRARY


def _basenames(paths: list[str]) -> set[str]:
    return {Path(p).name.lower() for p in paths}


def _qt_prefix_and_suffix(names: set[str]) -> tuple[str, str]:
    """Derive (qt_prefix, dll_suffix) from the library's own import closure,
    so the same assertions hold for Qt5/Qt6 and Debug/Release builds
    (Debug Qt kits append a trailing 'd' to every Qt DLL name)."""
    for n in sorted(names):
        for prefix in ("qt6core", "qt5core"):
            if n.startswith(prefix):
                return prefix[:-4], n[len(prefix):-len(".dll")]
    raise AssertionError(f"no QtCore in closure: {sorted(names)}")


def test_list_deps_direct_and_transitive(injector_and_library):
    """Closure must include direct deps AND Qt<major>Quick's transitive
    deps (Qt<major>Qml/Qt<major>QmlModels/Qt<major>Network) — the exact
    set that must be preloaded because the loader cannot find them via
    the library dir."""
    _, lib = injector_and_library
    deps = _run_list_deps(lib, [lib.parent])
    names = _basenames(deps)
    p, s = _qt_prefix_and_suffix(names)

    # Direct imports of the library.
    assert f"{p}widgets{s}.dll" in names
    assert f"{p}quick{s}.dll" in names
    assert f"{p}gui{s}.dll" in names
    assert f"{p}core{s}.dll" in names

    # Transitive deps of Qt<major>Quick, resolvable from the same dir.
    assert f"{p}qml{s}.dll" in names
    assert f"{p}qmlmodels{s}.dll" in names
    assert f"{p}network{s}.dll" in names

    # No duplicates.
    assert len(names) == len(deps), f"duplicate deps: {deps}"


def test_list_deps_excludes_system_dlls(injector_and_library):
    """System DLLs (kernel32, user32, CRT) are not in the search dir and
    must not be preloaded — the process/system resolves them."""
    _, lib = injector_and_library
    deps = _run_list_deps(lib, [lib.parent])
    names = _basenames(deps)
    for system in ("kernel32.dll", "user32.dll", "msvcp140.dll"):
        assert system not in names, f"{system} must not be preloaded"


def test_list_deps_search_dir_missing(injector_and_library, tmp_path):
    """With an empty search dir the closure is empty (everything is
    assumed process/system-resolvable)."""
    _, lib = injector_and_library
    deps = _run_list_deps(lib, [tmp_path])
    assert deps == []


def test_list_deps_bad_dll():
    if not INJECTOR.exists():
        pytest.skip("qt-injector.exe not built")
    proc = subprocess.run(
        [str(INJECTOR), "--list-deps", str(REPO / "nonexistent.dll")],
        capture_output=True, text=True, timeout=30)
    assert proc.returncode == 2
    assert "not found" in proc.stderr
