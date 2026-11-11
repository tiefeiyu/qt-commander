"""Launch-time injection: preload the runtime and attach to what starts."""

from __future__ import annotations

import os
import plistlib
import subprocess
import sys

import pytest

from qt_commander import launch


def _platform(monkeypatch, value: str) -> None:
    monkeypatch.setattr(launch.sys, "platform", value)


def test_platform_capabilities(monkeypatch):
    _platform(monkeypatch, "linux")
    assert launch.supported() is True
    assert launch.preload_variable() == "LD_PRELOAD"
    _platform(monkeypatch, "darwin")
    assert launch.preload_variable() == "DYLD_INSERT_LIBRARIES"
    _platform(monkeypatch, "win32")
    assert launch.supported() is False
    assert launch.preload_variable() is None


def test_build_environment_prepends_and_keeps_the_existing_entry(monkeypatch):
    _platform(monkeypatch, "linux")
    env = launch.build_environment(
        "/opt/lib.so", base={"LD_PRELOAD": "/old.so", "PATH": "/bin"}
    )
    assert env["LD_PRELOAD"] == "/opt/lib.so" + os.pathsep + "/old.so"
    assert env["PATH"] == "/bin"
    assert launch.build_environment("/opt/lib.so", base={})["LD_PRELOAD"] == (
        "/opt/lib.so"
    )
    assert launch.build_environment(
        "/opt/lib.so", base={}, extra={"QT_COMMANDER_RENDEZVOUS": "/tmp/r"}
    )["QT_COMMANDER_RENDEZVOUS"] == "/tmp/r"


def test_build_environment_is_unsupported_on_windows(monkeypatch):
    _platform(monkeypatch, "win32")
    with pytest.raises(launch.LaunchError) as info:
        launch.build_environment("x.dll", base={})
    assert info.value.code == "E_LAUNCH_UNSUPPORTED"


def _make_bundle(tmp_path, executable: str = "MyApp", plist: bool = True):
    contents = tmp_path / "My App.app" / "Contents"
    macos = contents / "MacOS"
    macos.mkdir(parents=True)
    inner = macos / executable
    inner.write_bytes(b"\xcf\xfa\xed\xfe")
    inner.chmod(0o755)
    if plist:
        (contents / "Info.plist").write_bytes(
            plistlib.dumps({"CFBundleExecutable": executable})
        )
    return contents.parent, inner


def test_resolve_app_bundle_uses_info_plist(tmp_path):
    bundle, inner = _make_bundle(tmp_path)
    assert launch.resolve_executable(bundle) == inner
    # A plain executable is returned unchanged.
    assert launch.resolve_executable(inner) == inner


def test_resolve_app_bundle_falls_back_to_a_single_executable(tmp_path):
    bundle, inner = _make_bundle(tmp_path, plist=False)
    assert launch.resolve_executable(bundle) == inner


def test_resolve_app_bundle_rejects_ambiguity(tmp_path):
    bundle, _ = _make_bundle(tmp_path, plist=False)
    second = bundle / "Contents" / "MacOS" / "Other"
    second.write_bytes(b"\xcf\xfa\xed\xfe")
    second.chmod(0o755)
    with pytest.raises(launch.LaunchError) as info:
        launch.resolve_executable(bundle)
    assert info.value.code == "E_LAUNCH_AMBIGUOUS"


@pytest.mark.skipif(os.name == "nt", reason="POSIX path semantics")
def test_sip_protected_paths():
    assert launch.sip_protected("/usr/bin/qt-app") is True
    assert launch.sip_protected("/System/Applications/App.app") is True
    assert launch.sip_protected("/Users/me/App.app/Contents/MacOS/App") is False


@pytest.mark.skipif(os.name == "nt", reason="POSIX path semantics")
def test_launch_refuses_sip_protected_target_on_macos(monkeypatch, tmp_path):
    _platform(monkeypatch, "darwin")
    library = tmp_path / "lib.dylib"
    library.write_bytes(b"")
    with pytest.raises(launch.LaunchError) as info:
        launch.launch("/usr/bin/true", library=library)
    assert info.value.code == "E_LAUNCH_SIP"


def test_launch_rejects_a_missing_library(monkeypatch, tmp_path):
    _platform(monkeypatch, "linux")
    with pytest.raises(launch.LaunchError) as info:
        launch.launch("/bin/true", library=tmp_path / "missing.so")
    assert info.value.code == "E_LAUNCH_NO_LIBRARY"


def test_launch_rejects_a_missing_target(monkeypatch, tmp_path):
    _platform(monkeypatch, "linux")
    library = tmp_path / "lib.so"
    library.write_bytes(b"")
    with pytest.raises(launch.LaunchError) as info:
        launch.launch(tmp_path / "nope", library=library)
    assert info.value.code == "E_LAUNCH_NO_TARGET"


def test_launch_is_unsupported_on_windows(monkeypatch, tmp_path):
    _platform(monkeypatch, "win32")
    library = tmp_path / "x.dll"
    library.write_bytes(b"")
    with pytest.raises(launch.LaunchError) as info:
        launch.launch("app.exe", library=library)
    assert info.value.code == "E_LAUNCH_UNSUPPORTED"


@pytest.mark.skipif(not launch.supported(), reason="needs a preload platform")
def test_launch_starts_a_process(tmp_path):
    """The preload variable is set and the target still runs."""
    library = tmp_path / "empty.so"
    library.write_bytes(b"")
    process = launch.launch("/bin/true", library=library, detach=False)
    assert process.wait(timeout=15) == 0


def test_tool_registration_matches_the_platform():
    """The MCP surface must describe what the platform can actually do."""
    from qt_commander import server

    tools = {tool.name for tool in _list_tools()}
    assert ("qt_launch" in tools) is server.LAUNCH_SUPPORTED
    instructions = server.mcp.instructions or ""
    assert ("qt_launch" in instructions) is server.LAUNCH_SUPPORTED


def _list_tools():
    import asyncio

    from qt_commander import server

    return asyncio.run(server.mcp.list_tools())


def test_launch_module_has_no_windows_dependency():
    """Importing the module must not require POSIX-only APIs."""
    import importlib

    reloaded = importlib.reload(launch)
    assert reloaded.__doc__
