"""Artifact layout and manifest matching."""

from __future__ import annotations

import json

from qt_commander import artifacts


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
