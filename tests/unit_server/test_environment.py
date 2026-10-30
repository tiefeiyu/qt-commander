"""Tests for lightweight Qt/toolchain discovery."""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from qt_commander import environment


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("", encoding="utf-8")


def test_detect_qt_kits_under_explicit_root(tmp_path: Path):
    _touch(tmp_path / "5.15.2" / "msvc2019_64" / "lib" / "cmake" / "Qt5" / "Qt5Config.cmake")
    _touch(tmp_path / "6.8.3" / "mingw_64" / "bin" / "qmake.exe")

    found = environment.detect_qt_environments([tmp_path])
    assert [item["version"] for item in found] == ["6.8.3", "5.15.2"]
    assert found[0]["major"] == 6
    assert found[0]["kits"][0]["compiler"] == "mingw"
    assert found[1]["kits"][0]["compiler"] == "msvc"


def test_empty_root_has_no_environments(tmp_path: Path):
    (tmp_path / "not-a-version").mkdir()
    assert environment.detect_qt_environments([tmp_path]) == []


def test_kit_compiler_classification():
    assert environment._kit_compiler("msvc2022_64") == "msvc"
    assert environment._kit_compiler("mingw1310_64") == "mingw"
    assert environment._kit_compiler("gcc_64") == "gcc"
    assert environment._kit_compiler("clang_64") == "clang"
    assert environment._kit_compiler("android_arm64_v8a") == "unknown"


def test_detect_toolchains_reports_versions(monkeypatch):
    monkeypatch.setattr(
        environment.shutil, "which",
        lambda name: r"C:\tools\g++.exe" if name == "g++" else None,
    )
    monkeypatch.setattr(
        environment.subprocess, "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout="g++ (GCC) 13.1.0\n", stderr=""
        ),
    )
    tools = environment.detect_toolchains()
    assert tools == [
        {
            "name": "g++",
            "path": r"C:\tools\g++.exe",
            "version": "g++ (GCC) 13.1.0",
        }
    ]
