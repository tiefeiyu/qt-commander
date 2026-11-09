"""Focused tests for the MCP tool layer's descriptor validation."""
from __future__ import annotations

import asyncio
import json
import os
import time

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


@pytest.mark.asyncio
async def test_invalid_arguments_return_typed_error():
    result = await server.mcp.call_tool("qt_windows", {})
    payload = result.structured_content
    assert payload["ok"] is False
    assert payload["error"]["code"] == "E_INVALID_ARGUMENT"
    assert "qt_windows" in payload["error"]["message"]


@pytest.mark.asyncio
async def test_detach_purges_descriptor(tmp_path, monkeypatch):
    class _Client:
        async def close(self):
            return None

    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    path = tmp_path / "descriptor-1.json"
    path.write_text(json.dumps({"pid": 1, "session_id": "abcd"}))
    session = server.Session("abcd", 1, client=_Client(), hello=None,
                             descriptor=str(path))
    server.sessions.add(session)
    result = await server.qt_detach("abcd", purge=True)
    assert result["ok"] is True
    assert result["purged"] is True
    assert not path.exists()


def test_gc_descriptors_removes_only_dead(tmp_path, monkeypatch):
    monkeypatch.setattr(
        server, "_descriptor_candidates",
        lambda: list(tmp_path.glob("descriptor-*.json")),
    )
    dead = tmp_path / "descriptor-999999.json"
    dead.write_text(json.dumps({"pid": 999999, "session_id": "dead"}))
    alive = tmp_path / "descriptor-2.json"
    alive.write_text(json.dumps({"pid": os.getpid(), "session_id": "alive"}))
    old = time.time() - 3600
    os.utime(dead, (old, old))
    assert server._gc_descriptors() == 1
    assert not dead.exists()
    assert alive.exists()


@pytest.mark.asyncio
async def test_build_reuses_running_job(tmp_path, monkeypatch):
    (tmp_path / "CMakeLists.txt").write_text("")
    kit = tmp_path / "qt"
    (kit / "bin").mkdir(parents=True)
    monkeypatch.setattr(server, "detect_qt_environments", lambda: [{
        "version": "5.15.2",
        "major": 5,
        "kits": [{"name": "mingw81_64", "path": str(kit),
                  "compiler": "mingw"}],
    }])
    monkeypatch.setattr(server, "_mingw_compilers", lambda: ("gcc", "g++"))
    monkeypatch.setattr(server.artifacts, "compiler_version",
                        lambda *args, **kwargs: None)

    async def fake_run(args, timeout):
        await asyncio.sleep(0.2)
        return 0, "ok"

    monkeypatch.setattr(server, "_run_build_command", fake_run)
    build_dir = tmp_path / "b"
    first = await server.qt_build(str(tmp_path), build_dir=str(build_dir),
                                  wait_s=0)
    assert first["status"] == "running"
    second = await server.qt_build(str(tmp_path), build_dir=str(build_dir),
                                   wait_s=0)
    assert second["job_id"] == first["job_id"]
    await asyncio.sleep(0.6)
    status = await server.qt_build_status(first["job_id"])
    job = status["jobs"][0]
    assert job["status"] == "ok"
    assert job["result"]["artifact_dir"].endswith("qt-5.15.2-release")


@pytest.mark.asyncio
async def test_query_reports_hidden_matches_on_miss():
    from qt_commander import ops

    calls: list[dict] = []
    replies = [{"count": 0, "tree": []}, {"count": 2}]

    class _Session:
        async def call(self, method, params, **kwargs):
            calls.append(params)
            return replies[len(calls) - 1]

    result = await ops.query(_Session(), where={"object_name": "hiddenBtn"})
    assert result["hidden_matched"] == 2
    assert "include_hidden=true" in result["hint"]
    assert calls[1]["include_hidden"] is True
    assert calls[1]["count_only"] is True


@pytest.mark.asyncio
async def test_query_does_not_probe_on_hit():
    from qt_commander import ops

    calls: list[dict] = []

    class _Session:
        async def call(self, method, params, **kwargs):
            calls.append(params)
            return {"count": 1, "tree": []}

    await ops.query(_Session(), where={"object_name": "visibleBtn"})
    assert len(calls) == 1
