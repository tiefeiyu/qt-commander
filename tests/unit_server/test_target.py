"""Target architecture metadata tests (no real injection)."""

from __future__ import annotations

import json
import os
import struct
import types

import psutil
import pytest

from qt_commander import server, target


@pytest.fixture(autouse=True)
def _module_scan_off(monkeypatch):
    """Keep the suite fast: module scanning is opt-in per test."""
    monkeypatch.setenv("QT_COMMANDER_PROCESS_MODULES", "0")
    target._MODULE_CACHE.clear()
    yield
    target._MODULE_CACHE.clear()


def _empty_runtime():
    return {
        "qt_major": None,
        "qt_version": None,
        "debug": None,
        "toolchain": None,
        "evidence": [],
    }


def _write_fake_pe(path):
    data = bytearray(0x200)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    path.write_bytes(data)


def _write_fake_elf(path):
    data = bytearray(64)
    data[0:4] = b"\x7fELF"
    data[4] = 2  # 64-bit
    data[5] = 1  # little endian
    struct.pack_into("<H", data, 18, 62)  # x86-64
    path.write_bytes(data)


def test_inspect_pe_and_elf(tmp_path):
    pe = tmp_path / "target.exe"
    _write_fake_pe(pe)
    assert target.inspect_executable(pe)["arch"] == "x64"
    assert target.inspect_executable(pe)["bits"] == 64

    elf = tmp_path / "target.elf"
    _write_fake_elf(elf)
    assert target.inspect_executable(elf)["arch"] == "x64"
    assert target.inspect_executable(elf)["format"] == "elf"


def test_descriptor_qt_version():
    info = target._descriptor_qt({"qt": {"major": 5, "version": "5.15.2"}})
    assert info["qt_major"] == 5
    assert info["qt_version"] == "5.15.2"


def test_list_processes_uses_descriptor(tmp_path):
    descriptor = tmp_path / f"descriptor-{os.getpid()}.json"
    descriptor.write_text(
        json.dumps({
            "pid": os.getpid(),
            "session_id": "s-test",
            "qt": {"major": 5, "version": "5.15.2"},
            "library_build_id": "qt5/GNU-AMD64/Debug",
        }),
        encoding="utf-8",
    )
    result = target.list_processes({os.getpid(): descriptor}, limit=200)
    assert result["total"] >= 1
    entry = next(p for p in result["processes"] if p["pid"] == os.getpid())
    assert entry["has_descriptor"] is True
    assert entry["qt_version"] == "5.15.2"
    assert entry["attach"]["descriptor"] == str(descriptor)


def test_descriptor_build_id_runtime():
    info = target._runtime_from_build_id("qt5/GNU-AMD64/Debug")
    assert info["qt_major"] == 5
    assert info["toolchain"] == "mingw"
    assert info["debug"] is True
    assert info["build_type"] == "Debug"


def test_adjacent_runtime_evidence(tmp_path):
    exe = tmp_path / "app.exe"
    _write_fake_pe(exe)
    (tmp_path / "Qt5Cored.dll").write_bytes(b"")
    (tmp_path / "msvcp140.dll").write_bytes(b"")
    info = target._adjacent_runtime(exe)
    assert info["qt_major"] == 5
    assert info["debug"] is True
    assert info["toolchain"] == "msvc"


def test_adjacent_runtime_detects_linux_qt_core(tmp_path):
    exe = tmp_path / "app"
    _write_fake_elf(exe)
    (tmp_path / "libQt5Core.so.5").write_bytes(b"")
    info = target._adjacent_runtime(exe)
    assert info["qt_major"] == 5
    assert any(
        path.endswith("libQt5Core.so.5") for path in info["evidence"]
    )


def test_list_processes_detects_loaded_qt_module(monkeypatch):
    qt_core = r"C:\Qt\5.15.2\mingw81_64\bin\Qt5Core.dll"
    monkeypatch.setenv("QT_COMMANDER_PROCESS_MODULES", "1")
    monkeypatch.setattr(target, "_adjacent_runtime", lambda exe: _empty_runtime())
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [types.SimpleNamespace(path=qt_core)],
    )
    result = target.list_processes(limit=1000)
    assert result["module_scan"]["enabled"] is True
    entry = next(p for p in result["processes"] if p["pid"] == os.getpid())
    assert entry["qt_major"] == 5
    assert entry["evidence"] == "module"


def test_list_processes_skips_process_without_qt(monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_PROCESS_MODULES", "1")
    monkeypatch.setattr(target, "_adjacent_runtime", lambda exe: _empty_runtime())
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [
            types.SimpleNamespace(path=r"C:\Windows\System32\ntdll.dll")
        ],
    )
    result = target.list_processes(limit=1000)
    assert result["count"] == 0
    assert all(p["pid"] != os.getpid() for p in result["processes"])
    assert "include_non_qt=true" in result["hint"]


@pytest.mark.asyncio
async def test_target_info_for_current_process():
    result = await server.qt_target_info(pid=os.getpid())
    assert result["pid"] == os.getpid()
    assert result["arch"]
    assert result["bits"] in {32, 64}
    assert result["confidence"] in {"high", "medium", "low"}


def test_scan_runtime_dirs_reports_ambiguous_flavour(tmp_path):
    """A directory holding both Qt runtimes cannot name the loaded flavour."""
    (tmp_path / "Qt5Core.dll").write_bytes(b"")
    (tmp_path / "Qt5Cored.dll").write_bytes(b"")
    info = target._scan_runtime_dirs([tmp_path])
    assert info["qt_major"] == 5
    assert info["debug"] is None
    assert info.get("flavor_ambiguous") is True


def test_scan_runtime_dirs_keeps_a_single_flavour(tmp_path):
    (tmp_path / "Qt5Cored.dll").write_bytes(b"")
    (tmp_path / "msvcp140d.dll").write_bytes(b"")
    info = target._scan_runtime_dirs([tmp_path])
    assert info["debug"] is True
    assert "flavor_ambiguous" not in info
    assert info["toolchain"] == "msvc"


def test_target_info_prefers_loaded_runtime_module(monkeypatch):
    """The mapped Qt runtime wins over a loader PATH that ships both DLLs."""
    monkeypatch.setenv("QT_COMMANDER_PROCESS_MODULES", "1")
    target._MODULE_CACHE.clear()
    monkeypatch.setattr(target, "_adjacent_runtime", lambda exe: _empty_runtime())
    monkeypatch.setattr(target, "_path_runtime", lambda proc: {
        "qt_major": 5,
        "qt_version": "5.15.2",
        "debug": True,
        "toolchain": "msvc",
        "evidence": [r"C:\Qt\5.15.2\msvc2019_64\bin\Qt5Cored.dll"],
    })
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [types.SimpleNamespace(path=r"C:\Qt\5.15.2\msvc2019_64\bin\Qt5Core.dll")],
    )
    info = target.target_info(pid=os.getpid())
    assert info["debug"] is False
    assert info["detected_from"] == "module"


def _make_release_artifact(tmp_path):
    from qt_commander import artifacts

    directory = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.44.35207",
        qt_version="5.15.2",
        qt_debug=False,
        root=tmp_path,
    )
    artifacts.write_manifest(
        directory,
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.44.35207",
        qt_version="5.15.2",
        qt_debug=False,
        build_type="Release",
        injector="bin/qtc-inject.exe",
        library="lib/qt_commander_inject.dll",
    )
    library = directory / "lib" / "qt_commander_inject.dll"
    library.parent.mkdir(parents=True, exist_ok=True)
    library.write_bytes(b"")
    return directory, library


def test_loaded_injector_reports_artifact(tmp_path, monkeypatch):
    directory, library = _make_release_artifact(tmp_path)
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [types.SimpleNamespace(path=str(library))],
    )
    monkeypatch.setattr(
        psutil.Process, "create_time",
        lambda self: library.stat().st_mtime + 10.0,
    )
    info = target.loaded_injector(os.getpid(), expected_artifact=directory)
    assert info["loaded"] is True
    assert info["artifact_dir"] == str(directory)
    assert info["build_id"] == "qt5.15.2/msvc-x64/Release"
    assert info["matches_expected"] is True
    assert info["stale"] is False


def test_loaded_injector_flags_stale_library(tmp_path, monkeypatch):
    directory, library = _make_release_artifact(tmp_path)
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [types.SimpleNamespace(path=str(library))],
    )
    # The process started before the library on disk was rebuilt.
    monkeypatch.setattr(
        psutil.Process, "create_time",
        lambda self: library.stat().st_mtime - 10.0,
    )
    info = target.loaded_injector(os.getpid(), expected_artifact=directory)
    assert info["stale"] is True
    assert target.injection_state(info, has_descriptor=True) == ("stale", True)


def test_loaded_injector_absent(monkeypatch):
    monkeypatch.setattr(
        psutil.Process,
        "memory_maps",
        lambda self: [types.SimpleNamespace(path=r"C:\Qt\bin\Qt5Core.dll")],
    )
    info = target.loaded_injector(os.getpid())
    assert info["loaded"] is False
    assert info["path"] is None
    assert target.injection_state(info) == ("none", False)


def test_injection_state_transitions():
    assert target.injection_state({"loaded": False}) == ("none", False)
    assert target.injection_state(
        {"loaded": True, "matches_expected": False}, has_descriptor=True
    ) == ("mismatch", True)
    assert target.injection_state(
        {"loaded": True, "matches_expected": True}, has_descriptor=False
    ) == ("loaded", True)
    assert target.injection_state(
        {"loaded": True, "matches_expected": True}, has_descriptor=True
    ) == ("loaded", False)


def test_target_info_reports_missing_artifact():
    info = target.target_info(pid=os.getpid())
    assert info["expected_artifact"] is None
    assert info["expected_artifact_error"]["error"] == "E_ARTIFACT_NOT_FOUND"
    assert info["injection_state"] in {"none", "loaded", "stale", "mismatch"}
    assert info["loaded_injector"]["loaded"] in {True, False}
