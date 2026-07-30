"""Test build state machine and source detection."""
import os
import pytest
from pathlib import Path
from mcp_server.builder import BuildState, check_build_state, detect_native_src, _sanitize_path_input


class TestSourceDetection:
    def test_env_var_priority(self, tmp_path, monkeypatch):
        monkeypatch.setenv("QT_COMMANDER_NATIVE_SRC", str(tmp_path))
        assert detect_native_src() == tmp_path

    def test_package_fallback(self, monkeypatch):
        monkeypatch.delenv("QT_COMMANDER_NATIVE_SRC", raising=False)
        src = detect_native_src()
        assert src.exists()


class TestBuildState:
    def test_not_built_when_no_artifacts(self, tmp_path, monkeypatch):
        monkeypatch.setenv("QT_COMMANDER_NATIVE_SRC", str(tmp_path))
        state = check_build_state(tmp_path)
        assert state == BuildState.NOT_BUILT

    def test_built_when_artifacts_present(self, tmp_path):
        build_dir = tmp_path / "injector" / "build"
        build_dir.mkdir(parents=True)
        (build_dir / "qt-injector.exe").write_text("fake")
        lib_dir = tmp_path / "library" / "build"
        lib_dir.mkdir(parents=True)
        (lib_dir / "libqt-commander.dll").write_text("fake")
        state = check_build_state(tmp_path)
        assert state in (BuildState.BUILT, BuildState.NOT_BUILT)


class TestSanitizePath:
    def test_valid_path(self):
        assert _sanitize_path_input("C:\\Qt\\qtenv2.bat", "qt_env") == "C:\\Qt\\qtenv2.bat"

    def test_newline_rejected(self):
        with pytest.raises(ValueError, match="Invalid character"):
            _sanitize_path_input("C:\\Qt\n\\evil.bat", "vcvars")

    def test_quote_rejected(self):
        with pytest.raises(ValueError, match="Quote character"):
            _sanitize_path_input('C:\\"evil.bat', "vcvars")
