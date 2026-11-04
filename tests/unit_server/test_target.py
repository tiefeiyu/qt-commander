"""Target architecture metadata tests (no real injection)."""

from __future__ import annotations

import os
import struct

import pytest

from qt_commander import server, target


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


@pytest.mark.asyncio
async def test_target_info_for_current_process():
    result = await server.qt_target_info(pid=os.getpid())
    assert result["pid"] == os.getpid()
    assert result["arch"]
    assert result["bits"] in {32, 64}
    assert result["confidence"] in {"high", "medium", "low"}
