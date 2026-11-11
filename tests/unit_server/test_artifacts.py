"""Artifact layout and manifest matching."""

from __future__ import annotations

import json

from qt_commander import artifacts


def _make_artifact(tmp_path, version, qt_debug=False):
    directory = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="mingw",
        compiler_version=version,
        qt_version="5.15.2",
        qt_debug=qt_debug,
        root=tmp_path,
    )
    artifacts.write_manifest(
        directory,
        os_name="windows",
        arch="x64",
        compiler="mingw",
        compiler_version=version,
        qt_version="5.15.2",
        qt_debug=qt_debug,
        build_type="Debug",
        injector="bin/qtc-inject.exe",
        library="lib/qt_commander_inject.dll",
    )
    return directory


def _target(**extra):
    return {
        "os": "windows",
        "arch": "x64",
        "qt_version": "5.15.2",
        "debug": False,
        "toolchain": "mingw",
        **extra,
    }


def test_build_dir_is_nested_and_compact(tmp_path):
    path = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.29.30153",
        qt_version="5.15.2",
        qt_debug=True,
        root=tmp_path,
    )
    assert path == (
        tmp_path / "windows-x64" / "msvc-19.29" / "qt-5.15.2-debug"
    )


def test_msvc_toolset_version_normalizes():
    assert artifacts.compiler_slug("msvc", "14.29.30133") == "msvc-19.29"
    assert artifacts.compiler_abbrev("msvc", "19.29.30153") == "19.29"


def test_find_matches_one_manifest(tmp_path):
    directory = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.29.30153",
        qt_version="5.15.2",
        qt_debug=True,
        root=tmp_path,
    )
    artifacts.write_manifest(
        directory,
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.29.30153",
        qt_version="5.15.2",
        qt_debug=True,
        build_type="Debug",
        injector="bin/qtc-inject.exe",
        library="lib/qt_commander_inject.dll",
    )
    found = artifacts.find(
        {
            "os": "windows",
            "arch": "x64",
            "qt_version": "5.15.2",
            "debug": True,
            "toolchain": "msvc",
        },
        root=tmp_path,
    )
    assert found["ok"] is True
    assert found["directory"] == directory
    assert json.loads((directory / "manifest.json").read_text())["qt"]["debug"]


def test_find_rejects_wrong_qt(tmp_path):
    directory = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.29",
        qt_version="6.8.3",
        qt_debug=False,
        root=tmp_path,
    )
    artifacts.write_manifest(
        directory,
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.29",
        qt_version="6.8.3",
        qt_debug=False,
        build_type="Release",
        injector="bin/qtc-inject.exe",
        library="lib/qt_commander_inject.dll",
    )
    found = artifacts.find(
        {
            "os": "windows",
            "arch": "x64",
            "qt_version": "5.15.2",
            "debug": True,
            "toolchain": "msvc",
        },
        root=tmp_path,
    )
    assert found["ok"] is False
    assert found["error"] == "E_ARTIFACT_NOT_FOUND"


def test_find_prefers_versioned_over_unknown(tmp_path):
    good = _make_artifact(tmp_path, "13.1.0")
    _make_artifact(tmp_path, None)
    found = artifacts.find(_target(), root=tmp_path)
    assert found["ok"] is True
    assert found["directory"] == good


def test_find_prefers_exact_version(tmp_path):
    exact = _make_artifact(tmp_path, "13.1.0")
    _make_artifact(tmp_path, "14.2.0")
    found = artifacts.find(_target(compiler_version="13.1.0"), root=tmp_path)
    assert found["ok"] is True
    assert found["directory"] == exact


def test_find_stays_ambiguous_when_both_versioned(tmp_path):
    _make_artifact(tmp_path, "13.1.0")
    _make_artifact(tmp_path, "14.2.0")
    found = artifacts.find(_target(), root=tmp_path)
    assert found["ok"] is False
    assert found["error"] == "E_ARTIFACT_AMBIGUOUS"
    assert found["candidates"]
    assert "artifact_dir" in found["hint"]


def test_find_treats_unknown_flavour_as_ambiguous(tmp_path):
    """An unknown Debug/Release must never be silently resolved to release."""
    release = _make_artifact(tmp_path, "19.44.35207", qt_debug=False)
    debug = _make_artifact(tmp_path, "19.44.35207", qt_debug=True)
    found = artifacts.find(
        {
            "os": "windows",
            "arch": "x64",
            "qt_version": "5.15.2",
            "debug": None,
            "toolchain": "mingw",
            "compiler_version": "19.44.35207",
        },
        root=tmp_path,
    )
    assert found["ok"] is False
    assert found["error"] == "E_ARTIFACT_AMBIGUOUS"
    directories = {candidate["directory"] for candidate in found["candidates"]}
    assert directories == {str(release), str(debug)}
    assert "explicitly" in found["hint"]


def test_manifest_records_canonical_build_id(tmp_path):
    directory = _make_artifact(tmp_path, "19.44.35207")
    manifest = json.loads((directory / "manifest.json").read_text())
    assert manifest["schema"] == 2
    assert manifest["build_id"] == "qt5.15.2/mingw-x64/Debug"
    assert artifacts.manifest_build_id(manifest) == manifest["build_id"]


def test_manifest_build_id_is_derived_for_legacy_schema(tmp_path):
    legacy = {
        "schema": 1,
        "target": {"os": "windows", "arch": "x64"},
        "compiler": {"family": "msvc", "version": "19.44"},
        "qt": {"version": "5.15.2", "debug": False},
        "build_type": "Release",
    }
    assert artifacts.manifest_build_id(legacy) == "qt5.15.2/msvc-x64/Release"
    assert artifacts.manifest_build_id(None) is None


def _fake_build(tmp_path, *, with_binaries=True):
    build = tmp_path / "build"
    build.mkdir()
    if with_binaries:
        injector = build / "src" / "qtc-inject.exe"
        library = build / "src" / "qt_commander_inject.dll"
        for path in (injector, library):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"MZ")
    return build


def test_install_from_build_writes_artifact_layout(tmp_path):
    build = _fake_build(tmp_path)
    root = tmp_path / "artifacts"
    installed = artifacts.install_from_build(
        build,
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.44.35207",
        qt_version="5.15.2",
        qt_debug=False,
        build_type="Release",
        root=root,
    )
    assert installed["build_id"] == "qt5.15.2/msvc-x64/Release"
    artifact_dir = installed["artifact_dir"]
    assert artifact_dir == root / "windows-x64/msvc-19.44/qt-5.15.2-release"
    assert (artifact_dir / "bin/qtc-inject.exe").read_bytes() == b"MZ"
    assert (artifact_dir / "lib/qt_commander_inject.dll").is_file()
    manifest = json.loads(installed["manifest"].read_text())
    assert manifest["library"] == "lib/qt_commander_inject.dll"
    assert manifest["build_id"] == "qt5.15.2/msvc-x64/Release"


def test_install_from_build_reports_missing_binaries(tmp_path):
    installed = artifacts.install_from_build(
        _fake_build(tmp_path, with_binaries=False),
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version=None,
        qt_version="5.15.2",
        qt_debug=True,
        build_type="Debug",
        root=tmp_path / "artifacts",
    )
    assert installed["manifest"] is None
    assert installed["injector"] is None
    assert installed["library"] is None
