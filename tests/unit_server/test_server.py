"""Focused tests for the MCP tool layer's descriptor validation."""
from __future__ import annotations

import asyncio
import json
import os
import tempfile
import time
import types
from pathlib import Path

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


@pytest.mark.asyncio
async def test_status_reports_session_build_id(tmp_path, monkeypatch):
    """The library build id must be visible, not a permanently empty field."""
    class _Client:
        closed = False

        async def close(self):
            return None

    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    hello = types.SimpleNamespace(library_version="2.0.0", capabilities=())
    session = server.Session("s-build", os.getpid(), client=_Client(),
                             hello=hello, build_id="qt5/MSVC-AMD64/Release")
    server.sessions.add(session)
    try:
        result = await server.qt_status()
        entry = next(item for item in result["sessions"]
                     if item["session_id"] == "s-build")
        assert entry["build_id"] == "qt5/MSVC-AMD64/Release"
    finally:
        server.sessions.remove("s-build")


def _no_live_descriptors(pid=None):
    return []


@pytest.mark.asyncio
async def test_attach_reports_already_loaded_injector(tmp_path, monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    monkeypatch.setattr(server, "_live_descriptors", _no_live_descriptors)
    monkeypatch.setattr(
        server.target, "target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "os": "windows",
            "arch": "x64",
            "qt_major": 5,
            "qt_version": "5.15.2",
            "debug": False,
            "toolchain": "msvc",
            "loaded_injector": {
                "loaded": True,
                "path": "C:/artifacts/lib/qt_commander_inject.dll",
                "artifact_dir": "C:/artifacts",
                "build_id": "qt5.15.2/msvc-x64/Release",
                "matches_expected": True,
                "serving": False,
            },
            "expected_artifact": {"directory": "C:/artifacts"},
        },
    )
    result = await server.qt_attach(pid=1234, mode="injection")
    assert result["ok"] is False
    assert result["error"]["reason"] == "injector_already_loaded"
    assert result["error"]["restart_required"] is True
    assert "restart the target" in result["error"]["hint"]
    details = result["error"]["details"]
    assert details["loaded_injector"]["build_id"] == "qt5.15.2/msvc-x64/Release"
    assert details["target"]["qt_version"] == "5.15.2"


@pytest.mark.asyncio
async def test_attach_explains_runtime_flavor_mismatch(tmp_path, monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    artifact_dir = tmp_path / "artifact"
    artifact_dir.mkdir()
    server.artifacts.write_manifest(
        artifact_dir,
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.44.35207",
        qt_version="5.15.2",
        qt_debug=True,
        build_type="Debug",
        injector="bin/qtc-inject.exe",
        library="lib/qt_commander_inject.dll",
    )
    library = artifact_dir / "lib" / "qt_commander_inject.dll"
    library.parent.mkdir(parents=True)
    library.write_bytes(b"")
    status = Path(tempfile.gettempdir()) / "qtc-inject-status-1234.txt"
    status.unlink(missing_ok=True)
    monkeypatch.setattr(server, "_live_descriptors", _no_live_descriptors)
    monkeypatch.setattr(
        server.inject, "default_dll_path", lambda pid, artifact_dir=None: library
    )
    monkeypatch.setattr(
        server.inject, "default_injector_path", lambda pid, artifact_dir=None: None
    )
    monkeypatch.setattr(
        server.target, "target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "os": "windows",
            "arch": "x64",
            "qt_major": 5,
            "qt_version": "5.15.2",
            "debug": False,
            "toolchain": "msvc",
            "loaded_injector": {"loaded": False},
            "expected_artifact": {"directory": str(tmp_path / "release")},
        },
    )

    def fake_inject(pid, path, **kwargs):
        status.write_text("timeout waiting for QCoreApplication",
                          encoding="utf-8")

    monkeypatch.setattr(server.inject, "inject_library", fake_inject)
    try:
        result = await server.qt_attach(pid=1234, mode="injection")
    finally:
        status.unlink(missing_ok=True)
    assert result["error"]["reason"] == "qt_runtime_flavor_mismatch"
    assert result["error"]["restart_required"] is True
    assert "Debug" in result["error"]["hint"]
    assert result["error"]["details"]["artifact"]["qt_debug"] is True
    assert str(tmp_path / "release") in result["error"]["hint"]


@pytest.mark.asyncio
async def test_attach_reports_missing_artifact(tmp_path, monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_RENDEZVOUS", str(tmp_path))
    monkeypatch.setattr(server, "_live_descriptors", _no_live_descriptors)
    monkeypatch.setattr(
        server.target, "target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "os": "windows",
            "arch": "x64",
            "qt_version": "5.15.2",
            "debug": False,
            "toolchain": "msvc",
            "loaded_injector": {"loaded": False},
            "expected_artifact": None,
        },
    )
    monkeypatch.setattr(
        server.inject, "default_dll_path", lambda pid, artifact_dir=None: None
    )
    monkeypatch.setattr(
        server.inject, "artifact_detail",
        lambda pid, artifact_dir=None: {
            "ok": False,
            "error": "E_ARTIFACT_NOT_FOUND",
            "message": "no artifact matches the target",
        },
    )
    result = await server.qt_attach(pid=1234, mode="injection")
    assert result["error"]["reason"] == "artifact_missing"
    assert "qt_build(for_pid=" in result["error"]["hint"]
    assert result["error"]["details"]["artifact_lookup"]["error"] == (
        "E_ARTIFACT_NOT_FOUND"
    )


@pytest.mark.asyncio
async def test_build_requires_build_id_validation_not_implemented():
    result = await server.qt_attach(pid=1, mode="injection", build_id="x")
    assert result["error"]["code"] == "E_NOT_IMPLEMENTED"
    assert "loaded_injector" in result["error"]["hint"]


@pytest.mark.asyncio
async def test_build_defaults_source_dir_to_the_checkout(monkeypatch):
    """Omitting source_dir must resolve the shipped project, not error out."""
    monkeypatch.setattr(server, "detect_qt_environments", lambda: [])
    result = await server.qt_build(qt_major=5, toolchain="mingw")
    assert result["error"]["code"] == "E_BUILD_REQUIRED"
    assert "CMakeLists" not in result["error"]["message"]


@pytest.mark.asyncio
async def test_build_for_pid_derives_toolset_and_flavour(tmp_path, monkeypatch):
    old_vcvars = tmp_path / "vs2022" / "vcvars64.bat"
    new_vcvars = tmp_path / "vs2026" / "vcvars64.bat"
    for path in (old_vcvars, new_vcvars):
        path.parent.mkdir()
        path.write_text("")
    monkeypatch.setattr(server, "detect_qt_environments", lambda: [{
        "version": "5.15.2",
        "major": 5,
        "kits": [{"name": "msvc2019_64",
                  "path": "C:/Qt/5.15.2/msvc2019_64",
                  "compiler": "msvc"}],
    }])
    monkeypatch.setattr(server, "detect_toolchains", lambda: [
        {"name": "msvc", "version": "14.51.36231", "vcvars": str(new_vcvars)},
        {"name": "msvc", "version": "14.44.35207", "vcvars": str(old_vcvars)},
    ])
    monkeypatch.setattr(
        server.target, "target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "os": "windows",
            "arch": "x64",
            "qt_major": 5,
            "qt_version": "5.15.2",
            "debug": False,
            "toolchain": "msvc",
            "compiler_version": "19.44.35207",
            "loaded_injector": {"loaded": False},
            "expected_artifact": {"directory": "C:/artifacts"},
        },
    )

    async def fake_run(args, timeout):
        return 0, "ok"

    monkeypatch.setattr(server, "_run_build_command", fake_run)
    result = await server.qt_build(for_pid=1234, build_dir=str(tmp_path / "b"))
    assert result["ok"] is True
    assert "msvc-19.44" in result["artifact_dir"]
    assert result["artifact_dir"].endswith("qt-5.15.2-release")
    assert result["build_type"] == "Release"
    assert result["compiler_version"] == "14.44.35207"
    assert result["restart_required"] is False
    assert "qt_attach" in result["next_step"]


@pytest.mark.asyncio
async def test_build_for_pid_refuses_unknown_flavour(monkeypatch):
    monkeypatch.setattr(
        server.target, "target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "os": "windows",
            "arch": "x64",
            "qt_major": 5,
            "toolchain": "msvc",
            "debug": None,
            "loaded_injector": {"loaded": False},
            "expected_artifact": None,
        },
    )
    result = await server.qt_build(for_pid=1234)
    assert result["error"]["code"] == "E_INVALID_ARGUMENT"
    assert "build_type" in result["error"]["message"]
