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
    monkeypatch.setattr(environment, "_visual_studio_toolchains", lambda: [])
    monkeypatch.setattr(
        environment.shutil, "which",
        lambda name: r"C:\tools\g++.exe" if name == "g++" else None,
    )
    monkeypatch.setattr(
        environment.subprocess, "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout=b"g++ (GCC) 13.1.0\n", stderr=b""
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


def test_toolchain_version_prefers_the_line_with_a_version(monkeypatch):
    """cl prints usage to stdout and the version banner to stderr."""
    monkeypatch.setattr(environment, "_visual_studio_toolchains", lambda: [])
    monkeypatch.setattr(
        environment.shutil, "which",
        lambda name: r"C:\tools\cl.exe" if name == "cl" else None,
    )
    monkeypatch.setattr(
        environment.subprocess, "run",
        lambda *args, **kwargs: SimpleNamespace(
            stdout="用法: cl [ 选项... ] 文件名...\n".encode("gbk"),
            stderr=(
                "用于 x64 的 Microsoft (R) C/C++ 优化编译器 19.44.35228 版\n"
            ).encode("utf-8") + b"\x8d",
        ),
    )
    tools = environment.detect_toolchains()
    assert tools[0]["name"] == "cl"
    assert "19.44.35228" in tools[0]["version"]
