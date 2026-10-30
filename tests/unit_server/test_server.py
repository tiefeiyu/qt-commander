"""Focused tests for the MCP tool layer's descriptor validation."""
from __future__ import annotations

import json
import os

import pytest

from qt_commander import server


def _write(tmp_path, payload) -> str:
    path = tmp_path / "descriptor.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    if os.name != "nt":
        path.chmod(0o600)
    return str(path)


@pytest.mark.asyncio
async def test_attach_rejects_non_object_descriptor(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, []))
    assert result["ok"] is False
    assert result["error"]["code"] == "E_LINKED_INVALID"


@pytest.mark.asyncio
async def test_attach_rejects_non_object_protocol(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, {
        "schema": 2,
        "protocol": 2,
        "mode": "linked",
        "pid": 1,
        "port": 1234,
        "token": "t",
        "session_id": "s",
    }))
    assert result["error"]["code"] == "E_PROTOCOL"


@pytest.mark.asyncio
async def test_attach_rejects_missing_token(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, {
        "schema": 2,
        "protocol": {"major": 2},
        "mode": "linked",
        "pid": 1,
        "port": 1234,
        "session_id": "s",
    }))
    assert result["error"]["code"] == "E_LINKED_INVALID"


@pytest.mark.asyncio
async def test_attach_rejects_non_numeric_pid(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, {
        "schema": 2,
        "protocol": {"major": 2},
        "mode": "linked",
        "pid": "nope",
        "port": 1234,
        "token": "t",
        "session_id": "s",
    }))
    assert result["error"]["code"] == "E_LINKED_INVALID"


@pytest.mark.asyncio
async def test_status_lists_live_descriptor_info(tmp_path, monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    (tmp_path / "descriptor-123.json").write_text(json.dumps({
        "schema": 2,
        "pid": os.getpid(),
        "session_id": "abcd",
        "library_version": "2.0.0",
    }), encoding="utf-8")
    result = await server.qt_status()
    assert result["descriptor_info"][0]["pid_alive"] is True
    assert result["descriptor_info"][0]["session_id"] == "abcd"


@pytest.mark.asyncio
async def test_build_rejects_missing_cmakelists(tmp_path):
    result = await server.qt_build(str(tmp_path))
    assert result["error"]["code"] == "E_INVALID_ARGUMENT"


@pytest.mark.asyncio
async def test_build_rejects_invalid_build_type(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("", encoding="utf-8")
    result = await server.qt_build(str(tmp_path), build_type="Fast")
    assert result["error"]["code"] == "E_INVALID_ARGUMENT"


@pytest.mark.asyncio
async def test_build_rejects_non_x64_arch(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("", encoding="utf-8")
    result = await server.qt_build(str(tmp_path), arch="arm64")
    assert result["error"]["code"] == "E_NOT_IMPLEMENTED"


@pytest.mark.asyncio
async def test_build_msvc_requires_vcvars(tmp_path, monkeypatch):
    (tmp_path / "CMakeLists.txt").write_text("", encoding="utf-8")
    monkeypatch.setattr(server, "detect_qt_environments", lambda: [{
        "version": "6.8.3",
        "major": 6,
        "kits": [{"name": "msvc2022_64", "path": "C:/Qt/6.8.3/msvc2022_64",
                  "compiler": "msvc"}],
    }])
    monkeypatch.setattr(server, "detect_toolchains", lambda: [])
    result = await server.qt_build(
        str(tmp_path), qt_major=6, toolchain="msvc"
    )
    assert result["error"]["code"] == "E_BUILD_REQUIRED"


@pytest.mark.asyncio
async def test_attach_rejects_stale_process_identity(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, {
        "schema": 2,
        "protocol": {"major": 2},
        "mode": "linked",
        "pid": os.getpid(),
        "process_start_time": 1.0,
        "port": 1234,
        "token": "t",
        "session_id": "s",
    }))
    assert result["error"]["code"] == "E_LINKED_STALE"


@pytest.mark.asyncio
async def test_attach_rejects_unknown_descriptor_mode(tmp_path):
    result = await server.qt_attach(descriptor=_write(tmp_path, {
        "schema": 2,
        "protocol": {"major": 2},
        "mode": "bogus",
        "pid": 1,
        "port": 1234,
        "token": "t",
        "session_id": "s",
    }))
    assert result["error"]["code"] == "E_LINKED_INVALID"
