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
