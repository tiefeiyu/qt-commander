"""Test environment_detector — VS/Qt build-tool discovery on Windows."""
import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest
from qt_commander.environment_detector import (
    _find_vcvars_scripts,
    _run_qmake_query,
    _find_qmake_on_path,
    _inspect_qmake_env,
    detect_vs_environments,
    detect_qt_environments,
)


# ═══════════════════════════════════════════════════════════════════════════
# Platform gate
# ═══════════════════════════════════════════════════════════════════════════

class TestPlatformGate:
    def test_detect_vs_returns_empty_on_non_windows(self, monkeypatch):
        monkeypatch.setattr(sys, "platform", "linux")
        assert detect_vs_environments() == []

    def test_detect_qt_returns_empty_on_non_windows(self, monkeypatch):
        monkeypatch.setattr(sys, "platform", "linux")
        assert detect_qt_environments() == []


# ═══════════════════════════════════════════════════════════════════════════
# _find_vcvars_scripts
# ═══════════════════════════════════════════════════════════════════════════

class TestFindVcvarsScripts:
    def test_empty_when_dir_missing(self, tmp_path):
        missing = tmp_path / "does_not_exist"
        assert _find_vcvars_scripts(missing) == []

    def test_finds_existing_scripts(self, tmp_path):
        build_aux = tmp_path / "VC" / "Auxiliary" / "Build"
        build_aux.mkdir(parents=True)
        (build_aux / "vcvars64.bat").write_text("@echo off")
        (build_aux / "vcvarsall.bat").write_text("@echo off")

        scripts = _find_vcvars_scripts(tmp_path)
        names = [s["name"] for s in scripts]
        assert "vcvars64.bat" in names
        assert "vcvarsall.bat" in names
        # Verify structure
        v64 = [s for s in scripts if s["name"] == "vcvars64.bat"][0]
        assert v64["arch"] == "x64"
        assert v64["path"] == str(build_aux / "vcvars64.bat")

    def test_returns_empty_for_empty_dir(self, tmp_path):
        build_aux = tmp_path / "VC" / "Auxiliary" / "Build"
        build_aux.mkdir(parents=True)
        assert _find_vcvars_scripts(tmp_path) == []


# ═══════════════════════════════════════════════════════════════════════════
# VS detection: vswhere + registry
# ═══════════════════════════════════════════════════════════════════════════

class TestDetectVsEnvironments:
    def test_vswhere_basic(self, monkeypatch, tmp_path):
        """Detects a VS installation via vswhere JSON output."""
        vs_root = tmp_path / "vs2022"
        build_aux = vs_root / "VC" / "Auxiliary" / "Build"
        build_aux.mkdir(parents=True)
        (build_aux / "vcvars64.bat").write_text("@echo off")

        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("qt_commander.environment_detector._vswhere_path",
                            lambda: tmp_path / "vswhere.exe")

        output = json.dumps([{
            "displayName": "Visual Studio 2022 Community",
            "installationVersion": "17.8.0",
            "installationPath": str(vs_root),
        }])
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = output
        monkeypatch.setattr("subprocess.run", mock_run)
        # Also mock modern registry to empty — we're testing vswhere
        monkeypatch.setattr(
            "qt_commander.environment_detector._detect_vs_modern_registry",
            lambda: [])

        results = detect_vs_environments()
        assert len(results) == 1

    def test_vswhere_missing_falls_back_to_registry(self, monkeypatch):
        """When vswhere is absent, registry detection is attempted."""
        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("qt_commander.environment_detector._vswhere_path",
                            lambda: None)
        monkeypatch.setattr(
            "qt_commander.environment_detector._detect_vs_modern_registry",
            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_vs_registry",
                            lambda: [])
        assert detect_vs_environments() == []

    def test_vswhere_timeout_returns_empty(self, monkeypatch):
        import subprocess as sp
        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("qt_commander.environment_detector._vswhere_path",
                            lambda: Path("/fake/vswhere.exe"))
        monkeypatch.setattr("subprocess.run",
                            MagicMock(side_effect=sp.TimeoutExpired("cmd", 30)))
        monkeypatch.setattr(
            "qt_commander.environment_detector._detect_vs_modern_registry",
            lambda: [])
        assert detect_vs_environments() == []

    def test_vswhere_non_zero_rc(self, monkeypatch, tmp_path):
        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("qt_commander.environment_detector._vswhere_path",
                            lambda: tmp_path / "vswhere.exe")
        mock_run = MagicMock()
        mock_run.return_value.returncode = 1
        mock_run.return_value.stdout = ""
        monkeypatch.setattr("subprocess.run", mock_run)
        monkeypatch.setattr(
            "qt_commander.environment_detector._detect_vs_modern_registry",
            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_vs_registry",
                            lambda: [])
        assert detect_vs_environments() == []

    def test_vswhere_no_vcvars_skipped(self, monkeypatch, tmp_path):
        """VS instance with no C++ tools is skipped."""
        vs_root = tmp_path / "vs_nocpp"
        vs_root.mkdir()  # No VC/Auxiliary/Build

        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("qt_commander.environment_detector._vswhere_path",
                            lambda: tmp_path / "vswhere.exe")

        output = json.dumps([{
            "displayName": "VS without C++",
            "installationPath": str(vs_root),
        }])
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = output
        monkeypatch.setattr("subprocess.run", mock_run)
        monkeypatch.setattr(
            "qt_commander.environment_detector._detect_vs_modern_registry",
            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_vs_registry",
                            lambda: [])
        assert detect_vs_environments() == []


# ═══════════════════════════════════════════════════════════════════════════
# _run_qmake_query
# ═══════════════════════════════════════════════════════════════════════════

class TestRunQmakeQuery:
    def test_parses_standard_output(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            "QT_INSTALL_PREFIX:C:/Qt/5.15.2/msvc2019_64\n"
            "QT_INSTALL_BINS:C:/Qt/5.15.2/msvc2019_64/bin\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)

        props = _run_qmake_query(Path("C:/Qt/5.15.2/msvc2019_64/bin/qmake.exe"))
        assert props is not None
        assert props["QT_INSTALL_PREFIX"] == "C:/Qt/5.15.2/msvc2019_64"
        assert props["QT_VERSION"] == "5.15.2"

    def test_non_zero_return(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 1
        mock_run.return_value.stdout = "error"
        monkeypatch.setattr("subprocess.run", mock_run)
        assert _run_qmake_query(Path("/fake/qmake.exe")) is None

    def test_empty_stdout(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = ""
        monkeypatch.setattr("subprocess.run", mock_run)
        assert _run_qmake_query(Path("/fake/qmake.exe")) is None

    def test_timeout(self, monkeypatch):
        import subprocess as sp
        monkeypatch.setattr("subprocess.run",
                            MagicMock(side_effect=sp.TimeoutExpired("qmake", 10)))
        assert _run_qmake_query(Path("/fake/qmake.exe")) is None

    def test_os_error(self, monkeypatch):
        monkeypatch.setattr("subprocess.run",
                            MagicMock(side_effect=OSError("not found")))
        assert _run_qmake_query(Path("/fake/qmake.exe")) is None

    def test_qt6_output(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            "QT_INSTALL_PREFIX:C:/Qt/6.5.0/msvc2019_64\n"
            "QT_VERSION:6.5.0\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)
        props = _run_qmake_query(Path("C:/Qt/6.5.0/msvc2019_64/bin/qmake.exe"))
        assert props is not None
        assert props["QT_VERSION"] == "6.5.0"


# ═══════════════════════════════════════════════════════════════════════════
# _inspect_qmake_env
# ═══════════════════════════════════════════════════════════════════════════

class TestInspectQmakeEnv:
    def test_qt5_with_qtenv2_bat(self, tmp_path, monkeypatch):
        """Qt5 install with qtenv2.bat next to qmake."""
        bin_dir = tmp_path / "5.15.2" / "msvc2019_64" / "bin"
        bin_dir.mkdir(parents=True)
        qmake = bin_dir / "qmake.exe"
        qtenv = bin_dir / "qtenv2.bat"
        qtenv.write_text("@echo off")

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            "QT_INSTALL_PREFIX:C:/Qt/5.15.2/msvc2019_64\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)

        info = _inspect_qmake_env(qmake)
        assert info is not None
        assert info["version"] == "5.15.2"
        assert info["compiler"] == "msvc2019_64"
        assert info["arch"] == "x64"
        assert info["qtenv_path"] == str(qtenv)
        assert info["qmake_path"] == str(qmake)

    def test_missing_install_prefix(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = "QT_VERSION:5.15.2\n"
        monkeypatch.setattr("subprocess.run", mock_run)

        assert _inspect_qmake_env(Path("/fake/qmake.exe")) is None

    def test_qmake_query_fails(self, monkeypatch):
        mock_run = MagicMock()
        mock_run.return_value.returncode = 1
        mock_run.return_value.stdout = ""
        monkeypatch.setattr("subprocess.run", mock_run)

        assert _inspect_qmake_env(Path("/fake/qmake.exe")) is None

    def test_x86_detected(self, tmp_path, monkeypatch):
        """32-bit Qt detected by compiler name."""
        bin_dir = tmp_path / "5.15.2" / "msvc2019" / "bin"
        bin_dir.mkdir(parents=True)
        qmake = bin_dir / "qmake.exe"

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            "QT_INSTALL_PREFIX:C:/Qt/5.15.2/msvc2019\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)

        info = _inspect_qmake_env(qmake)
        assert info is not None
        assert info["arch"] == "x86"


# ═══════════════════════════════════════════════════════════════════════════
# _find_qmake_on_path
# ═══════════════════════════════════════════════════════════════════════════

@pytest.mark.skipif(sys.platform != "win32", reason="Windows PATH/qmake detection")
class TestFindQmakeOnPath:
    def test_finds_qmake(self, tmp_path, monkeypatch):
        """Finds qmake.exe in a PATH entry."""
        bin_dir = tmp_path / "qt" / "bin"
        bin_dir.mkdir(parents=True)
        qmake_exe = bin_dir / "qmake.exe"
        qmake_exe.write_text("fake")

        monkeypatch.setattr("os.environ", {"PATH": str(bin_dir)})
        results = _find_qmake_on_path()
        assert Path(results[0]) == qmake_exe

    def test_empty_path_entries_skipped(self, monkeypatch):
        monkeypatch.setattr("os.environ", {"PATH": ";;;"})
        assert _find_qmake_on_path() == []

    def test_no_qmake_found(self, tmp_path, monkeypatch):
        empty_dir = tmp_path / "empty"
        empty_dir.mkdir()
        monkeypatch.setattr("os.environ", {"PATH": str(empty_dir)})
        assert _find_qmake_on_path() == []

    def test_deduplicates(self, tmp_path, monkeypatch):
        """Same directory appearing twice in PATH deduplicated."""
        bin_dir = tmp_path / "qt" / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "qmake.exe").write_text("fake")
        monkeypatch.setattr("os.environ",
                            {"PATH": f"{bin_dir};{bin_dir}"})
        results = _find_qmake_on_path()
        assert len(results) == 1


# ═══════════════════════════════════════════════════════════════════════════
# _detect_qt_structured  (C:\\Qt layout)
# ═══════════════════════════════════════════════════════════════════════════

class TestDetectQtStructured:
    def test_structured_layout(self, tmp_path, monkeypatch):
        """C:\\Qt with versioned compiler subdirs yields entries."""
        qt_root = tmp_path / "Qt"
        bin_dir = qt_root / "5.15.2" / "msvc2019_64" / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "qmake.exe").write_text("fake")
        (bin_dir / "qtenv2.bat").write_text("@echo off")

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            "QT_INSTALL_PREFIX:C:/Qt/5.15.2/msvc2019_64\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)
        # Patch the qt_root path so the global C:/Qt becomes tmp_path/Qt
        monkeypatch.setattr(
            "qt_commander.environment_detector.Path",
            lambda p: __import__("pathlib").Path(str(tmp_path / "Qt"))
            if p == "C:/Qt" else __import__("pathlib").Path(p),
        )
        from qt_commander.environment_detector import _detect_qt_structured
        results = _detect_qt_structured()
        assert len(results) >= 1
        assert results[0]["version"] == "5.15.2"

    def test_no_qt_root(self, tmp_path, monkeypatch):
        """Returns empty when C:\\Qt doesn't exist."""
        monkeypatch.setattr(
            "qt_commander.environment_detector.Path",
            lambda p: __import__("pathlib").Path(str(tmp_path / "no_such_dir")),
        )
        from qt_commander.environment_detector import _detect_qt_structured
        assert _detect_qt_structured() == []


# ═══════════════════════════════════════════════════════════════════════════
# _scan_drive_for_qmake  (fixed-drive kit layout scan)
# ═══════════════════════════════════════════════════════════════════════════

class TestScanDriveForQmake:
    def test_finds_kit_layout(self, tmp_path):
        """<root>/<ver>/<compiler>/bin/qmake.exe is found under any root."""
        kit = tmp_path / "Qt" / "5.15.2" / "msvc2019_64" / "bin"
        kit.mkdir(parents=True)
        (kit / "qmake.exe").write_text("fake")

        from qt_commander.environment_detector import _scan_drive_for_qmake
        found = _scan_drive_for_qmake(tmp_path)
        assert [p.parent.parent.name for p in found] == ["msvc2019_64"]

    def test_finds_deeper_custom_root(self, tmp_path):
        """Roots one level deeper (e.g. C:\\Software\\Qt) are still found."""
        kit = tmp_path / "Software" / "Qt" / "6.8.3" / "msvc2022_64" / "bin"
        kit.mkdir(parents=True)
        (kit / "qmake.exe").write_text("fake")

        from qt_commander.environment_detector import _scan_drive_for_qmake
        found = _scan_drive_for_qmake(tmp_path)
        assert [p.parent.parent.name for p in found] == ["msvc2022_64"]

    def test_skips_noise_dirs(self, tmp_path):
        """Well-known non-Qt directories are not probed."""
        for d in ("Windows", "Users", "Program Files", "ProgramData"):
            (tmp_path / d / "x" / "bin").mkdir(parents=True)

        from qt_commander.environment_detector import _scan_drive_for_qmake
        assert _scan_drive_for_qmake(tmp_path) == []

    def test_depth_limit(self, tmp_path):
        """Kits beyond max_depth are not reported."""
        kit = tmp_path / "a" / "b" / "c" / "d" / "e" / "bin"
        kit.mkdir(parents=True)
        (kit / "qmake.exe").write_text("fake")

        from qt_commander.environment_detector import _scan_drive_for_qmake
        assert _scan_drive_for_qmake(tmp_path, max_depth=3) == []
        assert len(_scan_drive_for_qmake(tmp_path, max_depth=6)) == 1


# ═══════════════════════════════════════════════════════════════════════════
# VS registry detection  (SxS key for VS 2017)
# ═══════════════════════════════════════════════════════════════════════════

@pytest.mark.skipif(sys.platform != "win32", reason="Windows-only registry tests")
class TestDetectVsRegistry:
    def test_sxs_key_vs2017(self, tmp_path, monkeypatch):
        """Registry SxS key yields VS 2017 entry."""
        vs_root = tmp_path / "vs2017"
        build_aux = vs_root / "VC" / "Auxiliary" / "Build"
        build_aux.mkdir(parents=True)
        (build_aux / "vcvars64.bat").write_text("@echo off")

        ed = "qt_commander.environment_detector.winreg"
        monkeypatch.setattr(f"{ed}.OpenKey",
                            lambda h, b, access=0, reserved=None: MagicMock())
        monkeypatch.setattr(f"{ed}.QueryValueEx",
                            lambda k, name: (str(vs_root), None))
        monkeypatch.setattr(f"{ed}.CloseKey", lambda k: None)

        from qt_commander.environment_detector import _detect_vs_registry
        results = _detect_vs_registry()
        assert len(results) >= 1
        assert "2017" in results[0]["display_name"]

    def test_registry_open_fails(self, monkeypatch):
        """When registry keys don't exist, returns empty."""
        ed = "qt_commander.environment_detector.winreg"
        monkeypatch.setattr(f"{ed}.OpenKey",
                            MagicMock(side_effect=FileNotFoundError()))
        monkeypatch.setattr(f"{ed}.CloseKey", lambda k: None)
        from qt_commander.environment_detector import _detect_vs_registry
        assert _detect_vs_registry() == []


# ═══════════════════════════════════════════════════════════════════════════
# Qt registry detection  (Trolltech keys)
# ═══════════════════════════════════════════════════════════════════════════

@pytest.mark.skipif(sys.platform != "win32", reason="Windows-only registry tests")
class TestDetectQtRegistry:
    def test_trolltech_registry(self, tmp_path, monkeypatch):
        """Trolltech registry key yields Qt entry."""
        qt_root = tmp_path / "Qt" / "5.15.2" / "msvc2019_64"
        bin_dir = qt_root / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "qmake.exe").write_text("fake")

        ed = "qt_commander.environment_detector.winreg"

        # OpenKey: top-level Versions key works, then subkey for version
        def mock_openkey(hive, base, access=0, reserved=None):
            return MagicMock()

        # EnumKey: first call returns "5.15.2", second raises OSError
        _enum_idx = 0
        def mock_enumkey(key, idx):
            nonlocal _enum_idx
            _enum_idx = idx
            if idx == 0:
                return "5.15.2"
            raise OSError("no more")

        # QueryValueEx: InstallDir on subkey returns bin_dir
        def mock_queryval(key, name):
            if name == "InstallDir":
                return (str(bin_dir), None)
            raise OSError("not found")

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            f"QT_INSTALL_PREFIX:{qt_root}\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)
        monkeypatch.setattr(f"{ed}.OpenKey", mock_openkey)
        monkeypatch.setattr(f"{ed}.EnumKey", mock_enumkey)
        monkeypatch.setattr(f"{ed}.QueryValueEx", mock_queryval)
        monkeypatch.setattr(f"{ed}.CloseKey", lambda k: None)

        from qt_commander.environment_detector import _detect_qt_registry
        results = _detect_qt_registry()
        assert len(results) >= 1
        assert results[0]["version"] == "5.15.2"

    def test_registry_key_not_found(self, monkeypatch):
        ed = "qt_commander.environment_detector.winreg"
        monkeypatch.setattr(f"{ed}.OpenKey",
                            MagicMock(side_effect=FileNotFoundError()))
        monkeypatch.setattr(f"{ed}.CloseKey", lambda k: None)
        from qt_commander.environment_detector import _detect_qt_registry
        assert _detect_qt_registry() == []


# ═══════════════════════════════════════════════════════════════════════════
# detect_qt_environments — integration
# ═══════════════════════════════════════════════════════════════════════════

@pytest.mark.skipif(sys.platform != "win32", reason="Windows-only Qt detection tests")
class TestDetectQtEnvironments:
    def test_qtdir_env_var(self, tmp_path, monkeypatch):
        """QTDIR env var is respected."""
        qt_root = tmp_path / "qt5"
        bin_dir = qt_root / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "qmake.exe").write_text("fake")
        (bin_dir / "qtenv2.bat").write_text("@echo off")

        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("os.environ", {"QTDIR": str(qt_root)})

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            f"QT_INSTALL_PREFIX:{qt_root}\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)

        results = detect_qt_environments()
        assert len(results) >= 1
        assert results[0]["version"] == "5.15.2"

    def test_qtdir_no_qmake(self, tmp_path, monkeypatch):
        """QTDIR pointing to non-Qt dir is ignored."""
        empty = tmp_path / "not_qt"
        empty.mkdir()
        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("os.environ", {"QTDIR": str(empty)})
        monkeypatch.setattr("qt_commander.environment_detector._find_qmake_on_path",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_registry",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_structured",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_drive_scan",
                            lambda: [])
        assert detect_qt_environments() == []

    def test_deduplicate_by_prefix(self, tmp_path, monkeypatch):
        """Same Qt found via multiple methods should appear once."""
        qt_root = tmp_path / "qt5"
        bin_dir = qt_root / "bin"
        bin_dir.mkdir(parents=True)
        qmake_exe = bin_dir / "qmake.exe"
        qmake_exe.write_text("fake")

        mock_run = MagicMock()
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = (
            f"QT_INSTALL_PREFIX:{qt_root}\n"
            "QT_VERSION:5.15.2\n"
        )
        monkeypatch.setattr("subprocess.run", mock_run)

        monkeypatch.setattr(sys, "platform", "win32")
        # QTDIR and PATH both point to same Qt
        monkeypatch.setattr("os.environ", {
            "QTDIR": str(qt_root),
            "PATH": str(bin_dir),
        })
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_registry",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_structured",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_drive_scan",
                            lambda: [])

        results = detect_qt_environments()
        # Should be deduped
        assert len(results) == 1

    def test_empty_on_no_qt(self, monkeypatch):
        monkeypatch.setattr(sys, "platform", "win32")
        monkeypatch.setattr("os.environ", {})
        monkeypatch.setattr("qt_commander.environment_detector._find_qmake_on_path",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_registry",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_structured",
                            lambda: [])
        monkeypatch.setattr("qt_commander.environment_detector._detect_qt_drive_scan",
                            lambda: [])
        assert detect_qt_environments() == []
