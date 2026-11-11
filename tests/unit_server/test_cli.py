"""CLI tests: doctor reporting and install-artifacts archive handling."""

from __future__ import annotations

import hashlib
import json
import os
import zipfile
from pathlib import Path

import pytest

from qt_commander import artifacts, cli


ARTIFACT_MEMBERS = {
    "windows-x64/msvc-19.44/qt-5.15.2-release/manifest.json": b'{"schema": 2}',
    "windows-x64/msvc-19.44/qt-5.15.2-release/bin/qtc-inject.exe": b"MZ",
    "windows-x64/msvc-19.44/qt-5.15.2-release/lib/qt_commander_inject.dll": b"MZ",
}


def make_zip(path: Path, members: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in members.items():
            archive.writestr(name, data)
    return path


def make_artifact(root: Path) -> Path:
    directory = artifacts.build_dir(
        os_name="windows",
        arch="x64",
        compiler="msvc",
        compiler_version="19.44.35207",
        qt_version="5.15.2",
        qt_debug=False,
        root=root,
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
    return directory


def test_parse_sums_accepts_standard_output():
    digest = "a" * 64
    sums = cli.parse_sums(
        f"{digest}  qt-commander-artifacts-win.zip\n"
        f"{'b' * 64} *starred.zip\n"
        "not a checksum line\n"
        f"{'c' * 63}  malformed-digest\n"
    )
    assert sums["qt-commander-artifacts-win.zip"] == digest
    assert sums["starred.zip"] == "b" * 64
    assert len(sums) == 2


def test_unsafe_members_are_rejected():
    assert cli._unsafe_member("../evil.txt") == "path traversal"
    assert cli._unsafe_member("/etc/passwd") == "absolute path"
    assert cli._unsafe_member("..\\evil.txt") == "path traversal"
    assert cli._unsafe_member("windows-x64/a/b") is None


def test_extract_installs_artifact_tree(tmp_path):
    archive = make_zip(tmp_path / "a.zip", ARTIFACT_MEMBERS)
    root = tmp_path / "artifacts"
    result = cli.extract_artifacts(archive, root)
    assert result["installed"] == 3
    assert result["directories"] == ["windows-x64"]
    installed = root / "windows-x64/msvc-19.44/qt-5.15.2-release/lib/qt_commander_inject.dll"
    assert installed.read_bytes() == b"MZ"


def test_extract_rejects_zip_slip(tmp_path):
    archive = make_zip(tmp_path / "evil.zip", {"../escape.txt": b"x"})
    with pytest.raises(ValueError, match="unsafe archive members"):
        cli.extract_artifacts(archive, tmp_path / "artifacts")
    assert not (tmp_path / "escape.txt").exists()


def test_extract_requires_target_shaped_tops(tmp_path):
    archive = make_zip(tmp_path / "flat.zip", {"readme.txt": b"x"})
    with pytest.raises(ValueError, match="<os>-<arch>"):
        cli.extract_artifacts(archive, tmp_path / "artifacts")


def test_extract_refuses_overwrite_without_force(tmp_path):
    archive = make_zip(tmp_path / "a.zip", ARTIFACT_MEMBERS)
    root = tmp_path / "artifacts"
    cli.extract_artifacts(archive, root)
    with pytest.raises(FileExistsError, match="--force"):
        cli.extract_artifacts(archive, root)
    assert cli.extract_artifacts(archive, root, force=True)["installed"] == 3


def _write_release_dir(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    zip_path = make_zip(directory / f"{cli.ASSET_PREFIX}windows-x64.zip",
                        ARTIFACT_MEMBERS)
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (directory / "SHA256SUMS").write_text(
        f"{digest}  {zip_path.name}\n", encoding="utf-8"
    )
    return zip_path


def test_install_from_dir_verifies_checksum(tmp_path, capsys):
    release = tmp_path / "release"
    _write_release_dir(release)
    root = tmp_path / "artifacts"
    code = cli.main(["install-artifacts", "--from-dir", str(release),
                     "--root", str(root)])
    assert code == 0
    assert list(root.glob("windows-x64/*/qt-5.15.2-release/manifest.json"))
    assert "installed" in capsys.readouterr().out


def test_install_from_dir_fails_on_bad_checksum(tmp_path, capsys):
    release = tmp_path / "release"
    zip_path = _write_release_dir(release)
    (release / "SHA256SUMS").write_text(
        f"{'0' * 64}  {zip_path.name}\n", encoding="utf-8"
    )
    root = tmp_path / "artifacts"
    code = cli.main(["install-artifacts", "--from-dir", str(release),
                     "--root", str(root)])
    assert code == 1
    assert not (root / "windows-x64").exists()
    assert "checksum mismatch" in capsys.readouterr().err


def test_install_from_dir_without_sums_warns(tmp_path, capsys):
    release = tmp_path / "release"
    release.mkdir()
    make_zip(release / f"{cli.ASSET_PREFIX}linux-x64.zip",
             {"linux-x64/gcc-13.3/qt-6.8.3-release/manifest.json": b"{}"})
    root = tmp_path / "artifacts"
    assert cli.main(["install-artifacts", "--from-dir", str(release),
                     "--root", str(root)]) == 0
    assert "no checksum published" in capsys.readouterr().err


def test_resolve_install_root_prefers_explicit():
    assert cli.resolve_install_root("C:/tmp/x") == Path("C:/tmp/x")


def test_doctor_json_reports_checks(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("QT_COMMANDER_ARTIFACTS", str(tmp_path / "artifacts"))
    make_artifact(tmp_path / "artifacts")
    monkeypatch.setattr("qt_commander.server._descriptor_candidates", lambda: [])
    code = cli.main(["doctor", "--json"])
    payload = json.loads(capsys.readouterr().out)
    names = [check["name"] for check in payload["checks"]]
    assert payload["ok"] is True
    assert code == 0
    assert {"python", "fastmcp", "psutil", "artifacts", "qt-kits"} <= set(names)
    artifact_check = next(c for c in payload["checks"] if c["name"] == "artifacts")
    assert artifact_check["status"] == "ok"
    assert "qt5.15.2/msvc-x64/Release" in artifact_check["detail"]


def test_doctor_warns_without_artifacts(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("QT_COMMANDER_ARTIFACTS", str(tmp_path / "empty"))
    monkeypatch.setattr("qt_commander.server._descriptor_candidates", lambda: [])
    code = cli.main(["doctor", "--json"])
    payload = json.loads(capsys.readouterr().out)
    artifact_check = next(c for c in payload["checks"] if c["name"] == "artifacts")
    assert artifact_check["status"] == "warn"
    assert artifact_check["hint"].startswith("run qt-commander install-artifacts")
    assert code == 0


def test_doctor_pid_reports_restart_required(monkeypatch, capsys):
    monkeypatch.setattr(
        "qt_commander.target.target_info",
        lambda pid=None, descriptor=None: {
            "pid": pid,
            "qt_major": 5,
            "qt_version": "5.15.2",
            "debug": False,
            "toolchain": "msvc",
            "detected_from": "module",
            "descriptor": {"library_version": "2.1.0"},
            "expected_artifact": {
                "directory": "C:/artifacts/release",
                "build_id": "qt5.15.2/msvc-x64/Release",
                "version": "0.9.0",
                "commit": "3760124abc",
            },
            "injection_state": "loaded",
            "restart_required": True,
            "loaded_injector": {
                "loaded": True,
                "build_id": "qt5.15.2/msvc-x64/Release",
                "artifact": {"version": "0.9.0", "commit": "3760124abc"},
            },
        },
    )
    code = cli.main(["doctor", "--pid", "1234", "--json"])
    payload = json.loads(capsys.readouterr().out)
    checks = {check["name"]: check for check in payload["checks"]}
    assert checks["target"]["status"] == "ok"
    assert "runtime 2.1.0" in checks["target"]["detail"]
    assert checks["target-artifact"]["status"] == "ok"
    assert "built from 0.9.0" in checks["target-artifact"]["detail"]
    assert "3760124a" in checks["target-artifact"]["detail"]
    assert checks["target-injector"]["status"] == "warn"
    assert "restart" in checks["target-injector"]["hint"]
    assert code == 0


def test_doctor_pid_failure_is_a_failing_check(monkeypatch, capsys):
    def explode(pid=None, descriptor=None):
        raise ValueError("cannot inspect pid 9: no such process")

    monkeypatch.setattr("qt_commander.target.target_info", explode)
    code = cli.main(["doctor", "--pid", "9", "--json"])
    payload = json.loads(capsys.readouterr().out)
    assert payload["ok"] is False
    assert code == 1


def test_version_flag_exits_zero(capsys):
    with pytest.raises(SystemExit) as info:
        cli.main(["--version"])
    assert info.value.code == 0
    assert "qt-commander" in capsys.readouterr().out


def test_artifact_root_env_override(tmp_path, monkeypatch):
    monkeypatch.setenv("QT_COMMANDER_ARTIFACTS", str(tmp_path))
    assert artifacts.default_root() == tmp_path
    monkeypatch.delenv("QT_COMMANDER_ARTIFACTS")
    assert artifacts.default_root() == artifacts.ROOT


@pytest.mark.parametrize(
    ("machine", "expected"),
    [
        ("AMD64", "x64"),
        ("x86_64", "x64"),
        ("aarch64", "arm64"),
        ("arm64", "arm64"),
        ("i686", "x86"),
        ("riscv64", "riscv64"),
    ],
)
def test_host_arch_maps_platform_machine(monkeypatch, machine, expected):
    monkeypatch.setattr(cli.platform, "machine", lambda: machine)
    assert cli.host_arch() == expected


def test_platform_asset_pattern_uses_artifact_slugs(monkeypatch):
    monkeypatch.setattr(cli.platform, "machine", lambda: "AMD64")
    monkeypatch.setattr(cli.artifacts, "os_slug", lambda: "windows")
    assert cli.platform_asset_pattern() == (
        "qt-commander-artifacts-windows-x64*.zip"
    )
