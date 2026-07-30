"""Test Session and SessionManager lifecycle."""
import pytest
from pathlib import Path
from mcp_server.session import Session, SessionManager
from mcp_server.errors import SessionExistsError


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / ".qt-commander"
    ws.mkdir()
    return ws


class TestSession:
    def test_session_init(self, workspace):
        sess = Session("abc123def456", 1234, Path("/tmp/test.dll"), workspace)
        assert sess.id == "abc123def456"
        assert sess.pid == 1234
        assert sess.connected is False
        assert sess.snapshot_count == 0

    def test_session_dir(self, workspace):
        sess = Session("test12345678", 5678, Path("/tmp/test.dll"), workspace)
        assert sess.session_dir == workspace / "sessions" / "test12345678"

    def test_write_metadata(self, workspace):
        sess = Session("meta12345678", 99, Path("/tmp/test.dll"), workspace)
        sess.port = 45678
        sess.token = "abcd" * 16
        sess.write_metadata()
        assert (sess.session_dir / "session.json").exists()


class TestSessionManager:
    @pytest.mark.asyncio
    async def test_create_session(self, workspace):
        sm = SessionManager(workspace)
        session = await sm.create(1234, Path("/tmp/lib.dll"))
        assert session.pid == 1234
        assert len(session.id) == 12
        assert sm.get(session.id) is session

    @pytest.mark.asyncio
    async def test_pid_dedup(self, workspace):
        sm = SessionManager(workspace)
        s1 = await sm.create(100, Path("/tmp/a.dll"))
        with pytest.raises(SessionExistsError):
            await sm.create(100, Path("/tmp/b.dll"))

    @pytest.mark.asyncio
    async def test_destroy_session(self, workspace):
        sm = SessionManager(workspace)
        s = await sm.create(200, Path("/tmp/a.dll"))
        ok = await sm.destroy(s.id, purge=True)
        assert ok is True
        assert sm.get(s.id) is None
        assert not s.session_dir.exists()

    @pytest.mark.asyncio
    async def test_list_sessions(self, workspace):
        sm = SessionManager(workspace)
        await sm.create(300, Path("/tmp/a.dll"))
        await sm.create(400, Path("/tmp/b.dll"))
        lst = sm.list_sessions()
        assert len(lst) == 2
