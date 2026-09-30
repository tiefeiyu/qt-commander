"""The scratch workspace must hide itself from the surrounding git repo.

The server creates .qt-commander/ (build outputs, per-session snapshots and
screenshots) inside whatever directory it is launched from.  Without a
.gitignore inside that directory, all of it appears as untracked clutter in
the user's `git status`.  server.py drops a self-ignoring .gitignore at
import time so the repo stays clean with no action from the user.
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class TestWorkspaceGitignore:
    def test_creates_gitignore_when_missing(self, tmp_path):
        """A workspace without a .gitignore gets one that ignores everything."""
        from qt_commander.server import _ensure_workspace_gitignored
        _ensure_workspace_gitignored(tmp_path)
        assert (tmp_path / ".gitignore").read_bytes() == b"*\n"

    def test_keeps_existing_gitignore(self, tmp_path):
        """A .gitignore the user already put there is never overwritten."""
        from qt_commander.server import _ensure_workspace_gitignored
        custom = b"!.keep-me\n*.tmp\n"
        (tmp_path / ".gitignore").write_bytes(custom)
        _ensure_workspace_gitignored(tmp_path)
        assert (tmp_path / ".gitignore").read_bytes() == custom

    def test_oserror_is_swallowed(self, tmp_path):
        """An unwritable location must not raise: the server still starts."""
        from qt_commander.server import _ensure_workspace_gitignored
        # A path whose parent is a regular file cannot exist, so creating
        # .gitignore inside it fails with NotADirectoryError (an OSError).
        # A read-only *directory* would not fail on Windows, so it is not
        # a usable failure mode here.
        blocker = tmp_path / "not-a-directory"
        blocker.write_text("regular file")
        _ensure_workspace_gitignored(blocker / "workspace")  # must not raise
        assert blocker.read_text() == "regular file"

    def test_server_import_writes_gitignore_into_configured_workspace(self, tmp_path):
        """Importing the server leaves the .gitignore behind at its workspace.

        The bootstrap runs at module import time, so this imports the server
        in a fresh interpreter with QT_COMMANDER_WORKSPACE pointed into
        tmp_path.  A reload in-process would leave the process-wide
        workspace (and its SessionManager) pointing at the test directory
        for every test that runs later in the same session.
        """
        workspace = tmp_path / "custom-workspace"
        env = {**os.environ, "QT_COMMANDER_WORKSPACE": str(workspace)}
        proc = subprocess.run(
            [sys.executable, "-c", "import qt_commander.server"],
            cwd=str(REPO), env=env, capture_output=True, text=True, timeout=60,
        )
        assert proc.returncode == 0, proc.stderr
        assert (workspace / ".gitignore").read_bytes() == b"*\n"
