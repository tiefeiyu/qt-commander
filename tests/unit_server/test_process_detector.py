"""Test Qt process detection."""
import sys
import pytest
from mcp_server.process_detector import list_qt_processes


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
            assert proc["arch"] in ("x64", "x86", "")

    def test_non_qt_excluded(self):
        result = list_qt_processes()
        for proc in result:
            assert proc["qt_version"] in ("5", "6", "")
