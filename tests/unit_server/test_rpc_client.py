"""Test inject_and_connect — subprocess, timeout, retry, eject cleanup."""
import asyncio
import json
import struct
from pathlib import Path
from unittest.mock import AsyncMock, patch

import pytest
from qt_commander.rpc_client import inject_and_connect
from qt_commander.errors import InjectionError
from qt_commander.session import Session


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / ".qt-commander"
    ws.mkdir()
    (ws / "sessions").mkdir()
    return ws


@pytest.fixture
def session(workspace):
    return Session("inject_test9", 9999, Path("/tmp/test.dll"), workspace)


@pytest.fixture
def injector_exe():
    return Path("/fake/qt-injector.exe")


@pytest.fixture
def port_file(tmp_path):
    return tmp_path / "port.txt"


# ============================================================================
# Success path
# ============================================================================

class TestInjectAndConnectSuccess:
    @pytest.mark.asyncio
    async def test_success(self, session, injector_exe, tmp_path):
        port_file = tmp_path / "port.txt"
        lib_path = Path("/tmp/libqt-commander.dll")

        # Mock injector subprocess: successful, outputs port+token JSON
        stdout_json = json.dumps({"port": 23456, "token": "abcd" * 16})
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (stdout_json.encode(), b"")
        mock_proc.returncode = 0

        # Mock TCP: reader returns auth OK
        mock_reader = asyncio.StreamReader()
        auth_ok = json.dumps({"jsonrpc": "2.0", "result": {}, "id": 1})
        frame = struct.pack("!I", len(auth_ok)) + auth_ok.encode()
        mock_reader.feed_data(frame)
        mock_reader.feed_eof()
        mock_writer = AsyncMock()

        with (
            patch("asyncio.create_subprocess_exec", return_value=mock_proc),
            patch("asyncio.open_connection", return_value=(mock_reader, mock_writer)),
        ):
            await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

        assert session.connected is True
        assert session.port == 23456
        assert session.token == "abcd" * 16

    @pytest.mark.asyncio
    async def test_writes_session_metadata(self, session, injector_exe, tmp_path):
        port_file = tmp_path / "port.txt"
        lib_path = Path("/tmp/libqt-commander.dll")

        stdout_json = json.dumps({"port": 34567, "token": "efgh" * 16})
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (stdout_json.encode(), b"")
        mock_proc.returncode = 0

        mock_reader = asyncio.StreamReader()
        auth_ok = json.dumps({"jsonrpc": "2.0", "result": {}, "id": 1})
        frame = struct.pack("!I", len(auth_ok)) + auth_ok.encode()
        mock_reader.feed_data(frame)
        mock_reader.feed_eof()
        mock_writer = AsyncMock()

        with (
            patch("asyncio.create_subprocess_exec", return_value=mock_proc),
            patch("asyncio.open_connection", return_value=(mock_reader, mock_writer)),
        ):
            await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

        # Verify session.json was written
        meta_file = session.session_dir / "session.json"
        assert meta_file.exists()
        meta = json.loads(meta_file.read_text())
        assert meta["pid"] == 9999
        assert meta["port"] == 34567


# ============================================================================
# Injector failure paths
# ============================================================================

class TestInjectorFailures:
    @pytest.mark.asyncio
    async def test_injector_nonzero_exit(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b"", b"error: access denied")
        mock_proc.returncode = 1

        with patch("asyncio.create_subprocess_exec", return_value=mock_proc):
            with pytest.raises(InjectionError, match="exited with code 1"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

    @pytest.mark.asyncio
    async def test_injector_timeout(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        mock_proc = AsyncMock()
        mock_proc.communicate.side_effect = asyncio.TimeoutError()

        mock_eject = AsyncMock()
        mock_eject.wait.return_value = None

        with (
            patch("asyncio.create_subprocess_exec", side_effect=[mock_proc, mock_eject]),
        ):
            with pytest.raises(InjectionError, match="timed out"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session, connect_timeout=0.01)

        # Verify eject was attempted after timeout
        mock_proc.kill.assert_called_once()

    @pytest.mark.asyncio
    async def test_injector_invalid_json(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b"not json", b"")
        mock_proc.returncode = 0

        with patch("asyncio.create_subprocess_exec", return_value=mock_proc):
            with pytest.raises(InjectionError, match="Failed to parse injector output"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

    @pytest.mark.asyncio
    async def test_injector_missing_port(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (json.dumps({"token": "x" * 64}).encode(), b"")
        mock_proc.returncode = 0

        with patch("asyncio.create_subprocess_exec", return_value=mock_proc):
            with pytest.raises(InjectionError, match="missing port or token"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

    @pytest.mark.asyncio
    async def test_injector_missing_token(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (json.dumps({"port": 12345}).encode(), b"")
        mock_proc.returncode = 0

        with patch("asyncio.create_subprocess_exec", return_value=mock_proc):
            with pytest.raises(InjectionError, match="missing port or token"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session)


# ============================================================================
# Connect retry path
# ============================================================================

class TestConnectRetry:
    @pytest.mark.asyncio
    async def test_connect_retry_then_succeed(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        stdout_json = json.dumps({"port": 45678, "token": "ijkl" * 16})
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (stdout_json.encode(), b"")
        mock_proc.returncode = 0

        # Fail twice, succeed on third
        mock_reader = asyncio.StreamReader()
        auth_ok = json.dumps({"jsonrpc": "2.0", "result": {}, "id": 1})
        frame = struct.pack("!I", len(auth_ok)) + auth_ok.encode()
        mock_reader.feed_data(frame)
        mock_reader.feed_eof()
        mock_writer = AsyncMock()

        with (
            patch("asyncio.create_subprocess_exec", return_value=mock_proc),
            patch("asyncio.open_connection", side_effect=[
                ConnectionRefusedError,
                ConnectionRefusedError,
                (mock_reader, mock_writer),
            ]) as mock_conn,
        ):
            await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

        assert mock_conn.call_count == 3
        assert session.connected is True

    @pytest.mark.asyncio
    async def test_connect_retry_all_fail(self, session, injector_exe, port_file):
        lib_path = Path("/tmp/lib.dll")
        stdout_json = json.dumps({"port": 56789, "token": "mnop" * 16})
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (stdout_json.encode(), b"")
        mock_proc.returncode = 0

        with (
            patch("asyncio.create_subprocess_exec", return_value=mock_proc),
            patch("asyncio.open_connection", side_effect=ConnectionRefusedError) as mock_conn,
        ):
            with pytest.raises(InjectionError, match="Failed to connect after 3 retries"):
                await inject_and_connect(9999, lib_path, port_file, injector_exe, session)

        assert mock_conn.call_count == 3
