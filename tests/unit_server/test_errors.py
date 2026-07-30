"""Test error code hierarchy and JSON formatting."""
from mcp_server.errors import (
    tool_error,
    BuildRequiredError,
    ElementStaleError,
    SessionExistsError,
    SessionNotFoundError,
)


class TestErrorCodes:
    def test_build_required(self):
        e = BuildRequiredError()
        assert e.code == 2001
        assert "build" in e.message.lower()

    def test_element_stale(self):
        e = ElementStaleError(42)
        assert e.code == 1002
        assert "42" in e.message

    def test_session_exists(self):
        e = SessionExistsError(1234, "abc")
        assert e.code == 2006
        assert "1234" in e.message

    def test_session_not_found(self):
        e = SessionNotFoundError("xyz")
        assert e.code == -32602

    def test_tool_error_format(self):
        result = tool_error(2003, "timeout")
        assert '"code": -32000' in result
        assert '"data"' in result
        assert '"code": 2003' in result
