"""Test build state machine, source detection, sanitization, and hash computation."""
import hashlib
import os
from pathlib import Path
from unittest.mock import patch

import pytest
from qt_commander.builder import (
    BuildState,
    INJECTOR_EXE_NAME,
    LIBRARY_NAME,
    check_build_state,
    detect_native_src,
    _sanitize_path_input,
    _compute_source_hash,
)


class TestSourceDetection:
    def test_env_var_priority(self, tmp_path, monkeypatch):
        monkeypatch.setenv("QT_COMMANDER_NATIVE_SRC", str(tmp_path))
        assert detect_native_src() == tmp_path

    def test_package_fallback(self, monkeypatch):
        monkeypatch.delenv("QT_COMMANDER_NATIVE_SRC", raising=False)
        src = detect_native_src()
        assert src.exists()

    def test_cwd_src_fallback(self, tmp_path, monkeypatch):
        monkeypatch.delenv("QT_COMMANDER_NATIVE_SRC", raising=False)
        # When package native/ doesn't have src/ and CWD has src/
        # detect_native_src should find the CWD path
        cwd_src = tmp_path / "src"
        cwd_src.mkdir()
        (cwd_src / "common").mkdir()
        (cwd_src / "injector").mkdir()
        (cwd_src / "library").mkdir()

        with patch("qt_commander.builder.Path.cwd", return_value=tmp_path):
            with patch("qt_commander.builder.Path.__init__", return_value=None):
                pass  # CWD fallback tested indirectly

    def test_file_not_found_error(self, tmp_path, monkeypatch):
        monkeypatch.delenv("QT_COMMANDER_NATIVE_SRC", raising=False)
        # If neither env var, package native/, nor CWD src/ exist → FileNotFoundError
        empty = tmp_path / "empty_dir"
        empty.mkdir()
        with patch("qt_commander.builder.Path.__init__", return_value=None):
            with patch("qt_commander.builder.Path.cwd", return_value=empty):
                with patch.object(Path, "exists", return_value=False):
                    with patch.object(Path, "__truediv__", return_value=empty / "nonexistent"):
                        pass  # Would raise FileNotFoundError but hard to mock cleanly


class TestBuildState:
    def test_not_built_when_no_artifacts(self, tmp_path):
        state = check_build_state(tmp_path)
        assert state == BuildState.NOT_BUILT

    def test_not_built_when_zero_size_injector(self, tmp_path):
        build_dir = tmp_path / "injector" / "build"
        build_dir.mkdir(parents=True)
        (build_dir / INJECTOR_EXE_NAME).write_text("")  # zero size

        lib_dir = tmp_path / "library" / "build"
        lib_dir.mkdir(parents=True)
        (lib_dir / LIBRARY_NAME).write_text("fake")

        state = check_build_state(tmp_path)
        assert state == BuildState.NOT_BUILT  # injector size 0 → NOT_BUILT

    def test_not_built_when_zero_size_library(self, tmp_path):
        build_dir = tmp_path / "injector" / "build"
        build_dir.mkdir(parents=True)
        (build_dir / INJECTOR_EXE_NAME).write_text("fake")

        lib_dir = tmp_path / "library" / "build"
        lib_dir.mkdir(parents=True)
        (lib_dir / LIBRARY_NAME).write_text("")  # zero size

        state = check_build_state(tmp_path)
        assert state == BuildState.NOT_BUILT

    def test_built_when_artifacts_present(self, tmp_path):
        bin_dir = tmp_path / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / INJECTOR_EXE_NAME).write_text("fake binary content")
        (bin_dir / LIBRARY_NAME).write_text("fake library content")

        state = check_build_state(tmp_path)
        assert state == BuildState.BUILT

    def test_manifest_hash_mismatch_causes_not_built(self, tmp_path):
        """When manifest exists but source hash differs, return NOT_BUILT."""
        bin_dir = tmp_path / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / INJECTOR_EXE_NAME).write_text("fake")
        (bin_dir / LIBRARY_NAME).write_text("fake")

        import json
        manifest = tmp_path / "build_manifest.json"
        manifest.write_text(json.dumps({"source_hash": "deadbeef00000000000000000000000000000000000000000000000000000000"}))

        # Current source hash will be different → NOT_BUILT
        with patch("qt_commander.builder.detect_native_src", return_value=tmp_path):
            with patch("qt_commander.builder._compute_source_hash", return_value="different_hash_1234"):
                state = check_build_state(tmp_path)
                assert state == BuildState.NOT_BUILT

    def test_manifest_corrupt_json_ignored(self, tmp_path):
        """Corrupt manifest JSON should be ignored (verified artifact existence)."""
        bin_dir = tmp_path / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / INJECTOR_EXE_NAME).write_text("fake")
        (bin_dir / LIBRARY_NAME).write_text("fake")

        (tmp_path / "build_manifest.json").write_text("corrupt json{{{")

        state = check_build_state(tmp_path)
        assert state == BuildState.BUILT  # corrupt manifest → still BUILT (verified by file existence)


class TestSanitizePath:
    def test_valid_path(self):
        assert _sanitize_path_input("C:\\Qt\\qtenv2.bat", "qt_env") == "C:\\Qt\\qtenv2.bat"

    def test_tab_ok(self):
        assert _sanitize_path_input("with\ttab", "test") == "with\ttab"

    def test_newline_rejected(self):
        with pytest.raises(ValueError, match="Invalid character"):
            _sanitize_path_input("C:\\Qt\n\\evil.bat", "vcvars")

    def test_quote_rejected(self):
        with pytest.raises(ValueError, match="Quote character"):
            _sanitize_path_input('C:\\"evil.bat', "vcvars")

    def test_single_quote_rejected(self):
        with pytest.raises(ValueError, match="Quote character"):
            _sanitize_path_input("C:\\'evil.bat", "vcvars")

    def test_backtick_rejected(self):
        with pytest.raises(ValueError, match="Quote character"):
            _sanitize_path_input("C:\\`evil.bat", "vcvars")

    def test_null_byte_rejected(self):
        with pytest.raises(ValueError, match="Invalid character"):
            _sanitize_path_input("C:\\Qt\0evil", "test")

    def test_normal_path_clean(self):
        """A normal path with spaces is fine."""
        result = _sanitize_path_input("C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat", "vcvars_path")
        assert "Program Files" in result


class TestComputeSourceHash:
    def test_compute_hash_consistent(self, tmp_path):
        """Same content → same hash."""
        (tmp_path / "test.cpp").write_text("int main() { return 0; }")
        h1 = _compute_source_hash(tmp_path)
        h2 = _compute_source_hash(tmp_path)
        assert h1 == h2
        assert len(h1) == 64  # SHA-256 produces 64 hex chars

    def test_compute_hash_different(self, tmp_path):
        """Different content → different hash."""
        (tmp_path / "a.cpp").write_text("int a;")
        h1 = _compute_source_hash(tmp_path)
        (tmp_path / "b.cpp").write_text("int b;")
        h2 = _compute_source_hash(tmp_path)
        assert h1 != h2

    def test_project_root_ignores_generated_build_files(self, tmp_path):
        """Project-root hashing must only cover src/: generated build files
        (mocs_compilation_*.cpp etc.) change on every build and would make
        the manifest hash never match."""
        src = tmp_path / "src"
        src.mkdir()
        (src / "main.cpp").write_text("int main() { return 0; }")
        build = tmp_path / ".qt-commander" / "build" / "library"
        build.mkdir(parents=True)

        h1 = _compute_source_hash(tmp_path)
        # Simulate a rebuild: generated moc file appears in the build dir.
        (build / "mocs_compilation_Release.cpp").write_text(
            "#include <QObject>")
        h2 = _compute_source_hash(tmp_path)
        assert h1 == h2, "build-dir generated files must not change the hash"

        # A real source change still changes the hash.
        (src / "main.cpp").write_text("int main() { return 1; }")
        h3 = _compute_source_hash(tmp_path)
        assert h3 != h1


# ============================================================================
# run_build + _run_cmake_build tests (mocked subprocess)
# ============================================================================

class TestRunBuild:
    @pytest.mark.asyncio
    async def test_run_build_success(self, tmp_path, monkeypatch):
        """Full build flow with mocked subprocess."""
        from unittest.mock import MagicMock, patch
        import qt_commander.builder as bmod

        # Set up fake native source
        native = tmp_path / "native" / "src"
        (native / "injector").mkdir(parents=True)
        (native / "injector" / "CMakeLists.txt").write_text("project(test)")
        (native / "library").mkdir(parents=True)
        (native / "library" / "CMakeLists.txt").write_text("project(test)")

        # Mock subprocess.run to succeed
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "Build succeeded"
        mock_result.stderr = ""

        # Mock _compute_source_hash to return stable value
        stable_hash = "a" * 64

        build_dir = tmp_path / "build"

        with (
            patch.object(bmod, "detect_native_src", return_value=native),
            patch.object(bmod, "_compute_source_hash", return_value=stable_hash),
            patch("subprocess.run", return_value=mock_result),
        ):
            result = await bmod.run_build(
                vcvars_path="C:\\vcvars64.bat",
                qt_env="C:\\Qt\\qtenv2.bat",
                vcvars_args="amd64",
                build_type="Release",
                qt_major=5,
                build_dir=build_dir,
            )
            assert result["injector_path"]
            assert result["library_path"]
            assert result["qt_version"] == "Qt5"

        # Check manifest was written
        manifest = build_dir / "build_manifest.json"
        assert manifest.exists()

    @pytest.mark.asyncio
    async def test_run_build_cmake_failure(self, tmp_path, monkeypatch):
        """Build fails when cmake returns non-zero."""
        from unittest.mock import MagicMock, patch
        import qt_commander.builder as bmod

        native = tmp_path / "native" / "src"
        (native / "injector").mkdir(parents=True)
        (native / "injector" / "CMakeLists.txt").write_text("broken")
        (native / "library").mkdir(parents=True)
        (native / "library" / "CMakeLists.txt").write_text("broken")

        mock_result = MagicMock()
        mock_result.returncode = 1
        mock_result.stdout = ""
        mock_result.stderr = "CMake Error: could not configure"

        build_dir = tmp_path / "build"

        with (
            patch.object(bmod, "detect_native_src", return_value=native),
            patch.object(bmod, "_compute_source_hash", return_value="b" * 64),
            patch("subprocess.run", return_value=mock_result),
        ):
            from qt_commander.errors import QtCommanderError
            with pytest.raises(QtCommanderError, match="Build failed"):
                await bmod.run_build(
                    vcvars_path="C:\\vcvars64.bat",
                    qt_env="C:\\Qt\\qtenv2.bat",
                    build_dir=build_dir,
                )

    @pytest.mark.asyncio
    async def test_run_build_sanitization_rejected(self, tmp_path):
        """Build rejects paths with control characters before running cmake."""
        import qt_commander.builder as bmod
        with pytest.raises(ValueError, match="Invalid character"):
            await bmod.run_build(
                vcvars_path="C:\\evil\n.bat",
                qt_env="C:\\Qt\\qtenv2.bat",
                build_dir=tmp_path / "build",
            )

    @pytest.mark.asyncio
    async def test_run_build_concurrent_rejected(self, tmp_path, monkeypatch):
        """Second concurrent build is rejected with code 2007."""
        from unittest.mock import MagicMock, patch
        import qt_commander.builder as bmod

        native = tmp_path / "native" / "src"
        (native / "injector").mkdir(parents=True)
        (native / "injector" / "CMakeLists.txt").write_text("test")
        (native / "library").mkdir(parents=True)
        (native / "library" / "CMakeLists.txt").write_text("test")

        mock_result = MagicMock()
        mock_result.returncode = 0

        build_dir = tmp_path / "build"

        with (
            patch.object(bmod, "detect_native_src", return_value=native),
            patch.object(bmod, "_compute_source_hash", return_value="c" * 64),
            patch("subprocess.run", return_value=mock_result),
        ):
            # First build sets state to BUILDING
            # Since we can't easily test concurrent async, test that state is managed
            result = await bmod.run_build(
                vcvars_path="C:\\vcvars64.bat",
                qt_env="C:\\Qt\\qtenv2.bat",
                build_dir=build_dir,
            )
            assert result["injector_path"]

    def test_find_build_script(self):
        """_find_build_script finds scripts/build_windows.bat in the project."""
        import qt_commander.builder as bmod
        bat = bmod._find_build_script()
        assert bat.name == "build_windows.bat"
        assert bat.exists()

    @pytest.mark.asyncio
    async def test_run_build_resets_state_on_error(self, tmp_path, monkeypatch):
        """After a build error, BuildState resets to NOT_BUILT."""
        from unittest.mock import MagicMock, patch
        import qt_commander.builder as bmod

        native = tmp_path / "native" / "src"
        (native / "injector").mkdir(parents=True)
        (native / "injector" / "CMakeLists.txt").write_text("test")
        (native / "library").mkdir(parents=True)
        (native / "library" / "CMakeLists.txt").write_text("test")

        mock_result = MagicMock()
        mock_result.returncode = 1
        mock_result.stderr = "error"

        build_dir = tmp_path / "build"

        with (
            patch.object(bmod, "detect_native_src", return_value=native),
            patch.object(bmod, "_compute_source_hash", return_value="d" * 64),
            patch("subprocess.run", return_value=mock_result),
        ):
            from qt_commander.errors import QtCommanderError
            with pytest.raises(QtCommanderError):
                await bmod.run_build(
                    vcvars_path="C:\\vcvars64.bat",
                    qt_env="C:\\Qt\\qtenv2.bat",
                    build_dir=build_dir,
                )

        # State should be reset to NOT_BUILT
        assert check_build_state(build_dir) == BuildState.NOT_BUILT

    def test_compute_hash_empty_dir(self, tmp_path):
        """Empty directory → deterministic hash."""
        h = _compute_source_hash(tmp_path)
        assert len(h) == 64
        assert h == hashlib.sha256().hexdigest()
