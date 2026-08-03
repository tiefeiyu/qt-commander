"""Test Qt process detection — real integration + deterministic mocks."""
import sys
from unittest.mock import MagicMock

import pytest
from qt_commander.process_detector import list_qt_processes, _check_qt_linux


class TestListQtProcesses:
    def test_returns_list(self):
        result = list_qt_processes()
        assert isinstance(result, list)
        for proc in result:
            assert "pid" in proc
            assert "name" in proc
            assert "qt_version" in proc
            assert isinstance(proc["pid"], int)
            assert proc["pid"] > 0

    @pytest.mark.skipif(sys.platform != "win32", reason="Windows only")
    def test_windows_arch_field(self):
        result = list_qt_processes()
        for proc in result:
            assert "arch" in proc
            assert proc["arch"] in ("x64", "x86", "arm64", "")

    def test_non_qt_excluded(self):
        result = list_qt_processes()
        for proc in result:
            assert proc["qt_version"] in ("5", "6", "")


# ============================================================================
# Deterministic mock tests
# ============================================================================

class TestProcessDetectorMocked:
    def test_empty(self, monkeypatch):
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [])
        assert list_qt_processes() == []

    def test_pid_none_skipped(self, monkeypatch):
        p = MagicMock()
        p.info = {"pid": None, "name": "x", "exe": None}
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        assert list_qt_processes() == []

    def test_access_denied_skipped(self, monkeypatch):
        p = MagicMock()
        p.info.__getitem__.side_effect = __import__('psutil').AccessDenied()
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        assert list_qt_processes() == []

    def test_no_such_process_skipped(self, monkeypatch):
        p = MagicMock()
        p.info.__getitem__.side_effect = __import__('psutil').NoSuchProcess(99)
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        assert list_qt_processes() == []

    def test_qt5_detected(self, monkeypatch):
        mmap = MagicMock()
        mmap.path = r"C:\Qt\5.15.2\bin\Qt5Core.dll"
        p = MagicMock()
        p.info = {"pid": 1234, "name": "app.exe", "exe": r"C:\app.exe"}
        p.name.return_value = "app.exe"
        p.memory_maps.return_value = [mmap]
        monkeypatch.setattr("sys.platform", "win32")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 1
        assert r[0]["pid"] == 1234
        assert r[0]["qt_version"] == "5"

    def test_qt6_detected(self, monkeypatch):
        mmap = MagicMock()
        mmap.path = r"C:\Qt\6.8.3\bin\Qt6Core.dll"
        p = MagicMock()
        p.info = {"pid": 5678, "name": "qt6app.exe", "exe": r"C:\qt6app.exe"}
        p.name.return_value = "qt6app.exe"
        p.memory_maps.return_value = [mmap]
        monkeypatch.setattr("sys.platform", "win32")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 1
        assert r[0]["qt_version"] == "6"

    def test_non_qt_filtered(self, monkeypatch):
        mmap = MagicMock()
        mmap.path = r"C:\Windows\System32\kernel32.dll"
        p = MagicMock()
        p.info = {"pid": 9999, "name": "notepad.exe", "exe": r"C:\notepad.exe"}
        p.name.return_value = "notepad.exe"
        p.memory_maps.return_value = [mmap]
        monkeypatch.setattr("sys.platform", "win32")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 0

    def test_memory_maps_access_denied(self, monkeypatch):
        p = MagicMock()
        p.info = {"pid": 1111, "name": "p.exe", "exe": r"C:\p.exe"}
        p.name.return_value = "p.exe"
        p.memory_maps.side_effect = __import__('psutil').AccessDenied()
        monkeypatch.setattr("sys.platform", "win32")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 0

    def test_linux_qt5(self, monkeypatch, tmp_path):
        maps = tmp_path / "maps"
        maps.write_text("7f000 r-xp /usr/lib/libQt5Core.so.5.15.2\n")
        real_open = open
        monkeypatch.setattr("builtins.open", lambda path, mode: real_open(str(maps), mode))
        is_qt, ver, arch = _check_qt_linux(1234)
        assert is_qt is True
        assert ver == "5"

    def test_linux_qt6(self, monkeypatch, tmp_path):
        maps = tmp_path / "maps"
        maps.write_text("7f000 r-xp /usr/lib/libQt6Core.so.6.8.3\n")
        real_open = open
        monkeypatch.setattr("builtins.open", lambda path, mode: real_open(str(maps), mode))
        is_qt, ver, arch = _check_qt_linux(5678)
        assert is_qt is True
        assert ver == "6"

    def test_linux_no_qt(self, monkeypatch, tmp_path):
        maps = tmp_path / "maps"
        maps.write_text("7f000 r-xp /usr/lib/libc.so.6\n")
        real_open = open
        monkeypatch.setattr("builtins.open", lambda path, mode: real_open(str(maps), mode))
        is_qt, ver, arch = _check_qt_linux(1234)
        assert is_qt is False

    def test_linux_permission_denied(self, monkeypatch):
        monkeypatch.setattr("builtins.open", lambda path, mode: (_ for _ in ()).throw(PermissionError()))
        is_qt, ver, arch = _check_qt_linux(1234)
        assert is_qt is False

    def test_linux_file_not_found(self, monkeypatch):
        monkeypatch.setattr("builtins.open", lambda path, mode: (_ for _ in ()).throw(FileNotFoundError()))
        is_qt, ver, arch = _check_qt_linux(1234)
        assert is_qt is False

    def test_macos_detected(self, monkeypatch):
        mmap = MagicMock()
        mmap.path = "/usr/local/lib/QtCore.framework/Versions/5/QtCore"
        p = MagicMock()
        p.info = {"pid": 4321, "name": "macapp", "exe": "/App/MacOS/MyApp"}
        p.name.return_value = "macapp"
        p.memory_maps.return_value = [mmap]
        monkeypatch.setattr("sys.platform", "darwin")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 1

    def test_macos_access_denied(self, monkeypatch):
        p = MagicMock()
        p.info = {"pid": 4444, "name": "x", "exe": "/bin/x"}
        p.name.return_value = "x"
        p.memory_maps.side_effect = __import__('psutil').AccessDenied()
        monkeypatch.setattr("sys.platform", "darwin")
        monkeypatch.setattr("qt_commander.process_detector.psutil.process_iter", lambda attrs: [p])
        r = list_qt_processes()
        assert len(r) == 0
