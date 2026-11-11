"""Packaging tooling: the release script and compiler version probing."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from qt_commander import artifacts

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "package_artifacts.py"


def _fake_build(tmp_path: Path) -> Path:
    build = tmp_path / "build"
    for relative in ("src/qtc-inject.exe", "src/qt_commander_inject.dll"):
        path = build / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"MZ")
    return build


def test_package_script_writes_github_output(tmp_path):
    build = _fake_build(tmp_path)
    artifact_root = tmp_path / "artifacts"
    archive_dir = tmp_path / "dist"
    output = tmp_path / "github_output.txt"
    output.write_text("", encoding="utf-8")
    env = dict(os.environ, GITHUB_OUTPUT=str(output))
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--build-dir", str(build),
            # Pin the OS: the default is the build host, and CI runs on Linux.
            "--os", "windows",
            "--qt-version", "5.15.2",
            "--toolchain", "msvc",
            "--compiler-version", "19.44.35207",
            "--build-type", "Release",
            "--qt-release",
            "--root", str(artifact_root),
            "--archive-dir", str(archive_dir),
            "--github-output",
            "--json",
        ],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    lines = output.read_text(encoding="utf-8")
    assert ("archive_name=qt-commander-artifacts-windows-x64-qt-5.15.2-release.zip"
            in lines)
    assert "relative_dir=windows-x64/msvc-19.44/qt-5.15.2-release" in lines
    assert "build_id=qt5.15.2/msvc-x64/Release" in lines
    manifest = artifact_root / "windows-x64/msvc-19.44/qt-5.15.2-release/manifest.json"
    assert manifest.is_file()

    import zipfile

    archive_path = archive_dir / "qt-commander-artifacts-windows-x64-qt-5.15.2-release.zip"
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
    assert all(name.startswith("windows-x64/") for name in names), names
    assert ("windows-x64/msvc-19.44/qt-5.15.2-release/bin/qtc-inject.exe"
            in names)


def test_package_script_fails_without_binaries(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    completed = subprocess.run(
        [sys.executable, str(SCRIPT), "--build-dir", str(build),
         "--qt-version", "5.15.2", "--toolchain", "msvc",
         "--compiler-version", "19.44", "--root", str(tmp_path / "artifacts")],
        cwd=str(ROOT), capture_output=True, text=True, check=False,
    )
    assert completed.returncode == 1
    assert "did the build succeed" in completed.stderr


def test_msvc_banner_is_parsed_without_a_version_flag(monkeypatch):
    calls: list[list[str]] = []

    class _Completed:
        stdout = (
            "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35228 for x64\n"
            "Copyright (C) Microsoft Corporation.  All rights reserved.\n"
        )
        stderr = ""
        returncode = 0

    def fake_run(command, **kwargs):
        calls.append(list(command))
        return _Completed()

    monkeypatch.setattr(artifacts.subprocess, "run", fake_run)
    assert artifacts.compiler_version("msvc", "cl") == "19.44.35228"
    assert calls[0] == ["cl"]
    assert artifacts.compiler_slug("msvc", "19.44.35228") == "msvc-19.44"


def test_unknown_compiler_reports_nothing(monkeypatch):
    monkeypatch.setattr(artifacts.shutil, "which", lambda name: None)
    assert artifacts.compiler_version("msvc") is None
