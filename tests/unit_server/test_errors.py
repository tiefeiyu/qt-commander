"""Test all error code classes and JSON formatting — 100% error class coverage."""
import json
from qt_commander.errors import (
    tool_error,
    QtCommanderError,
    ElementDestroyedError,
    ElementStaleError,
    ElementNotVisibleError,
    ElementDisabledError,
    ElementZeroSizeError,
    BuildRequiredError,
    InjectionError,
    RpcTimeoutError,
    MainThreadUnresponsiveError,
    DiskFullError,
    SessionExistsError,
    OperationInProgressError,
    FrameTooLargeError,
    AuthFailedError,
    SnapshotTruncatedError,
    SessionNotFoundError,
)


class TestAllErrorCodes:
    """Test that every error subclass instantiates with correct code."""

    def test_element_destroyed(self):
        e = ElementDestroyedError(42)
        assert e.code == 1001
        assert "42" in e.message

    def test_element_stale(self):
        e = ElementStaleError(99)
        assert e.code == 1002
        assert "99" in e.message

    def test_element_not_visible(self):
        e = ElementNotVisibleError(7)
        assert e.code == 1003
        assert "7" in e.message

    def test_element_disabled(self):
        e = ElementDisabledError(3)
        assert e.code == 1004
        assert "3" in e.message

    def test_element_zero_size(self):
        e = ElementZeroSizeError(1)
        assert e.code == 1005
        assert "1" in e.message

    def test_build_required(self):
        e = BuildRequiredError()
        assert e.code == 2001
        assert "build" in e.message.lower()

    def test_injection_error(self):
        e = InjectionError("access denied")
        assert e.code == 2002
        assert "access denied" in e.message

    def test_rpc_timeout(self):
        e = RpcTimeoutError("main thread blocked")
        assert e.code == 2003
        assert "main thread blocked" in e.message

    def test_rpc_timeout_no_detail(self):
        e = RpcTimeoutError()
        assert e.code == 2003
        assert "did not respond" in e.message

    def test_main_thread_unresponsive(self):
        e = MainThreadUnresponsiveError()
        assert e.code == 2004

    def test_disk_full(self):
        e = DiskFullError(100, 500)
        assert e.code == 2005
        assert "100" in e.message
        assert "500" in e.message

    def test_session_exists(self):
        e = SessionExistsError(1234, "abc")
        assert e.code == 2006
        assert "1234" in e.message
        assert "abc" in e.message

    def test_operation_in_progress(self):
        e = OperationInProgressError()
        assert e.code == 2007

    def test_frame_too_large(self):
        e = FrameTooLargeError(20000000)
        assert e.code == 2008
        assert "20000000" in e.message

    def test_auth_failed(self):
        e = AuthFailedError()
        assert e.code == 2009

    def test_snapshot_truncated(self):
        e = SnapshotTruncatedError("cycle_detected")
        assert e.code == 2010
        assert "cycle_detected" in e.message

    def test_session_not_found(self):
        e = SessionNotFoundError("xyz")
        assert e.code == -32602
        assert "xyz" in e.message

    def test_tool_error_format(self):
        result = tool_error(2003, "timeout")
        parsed = json.loads(result)
        assert parsed["error"]["code"] == -32000
        assert parsed["error"]["data"]["code"] == 2003
        assert "timeout" in parsed["error"]["message"]

    def test_base_class(self):
        e = QtCommanderError(9999, "custom error")
        assert e.code == 9999
        assert e.message == "custom error"

    def test_all_codes_unique(self):
        """Verify no duplicate error codes across all subclasses."""
        codes = {}
        classes_and_args = [
            (ElementDestroyedError, (42,)),
            (ElementStaleError, (42,)),
            (ElementNotVisibleError, (42,)),
            (ElementDisabledError, (42,)),
            (ElementZeroSizeError, (42,)),
            (BuildRequiredError, ()),
            (InjectionError, ("test detail",)),
            (RpcTimeoutError, ()),
            (MainThreadUnresponsiveError, ()),
            (DiskFullError, (100, 500)),
            (SessionExistsError, (42, "abc")),
            (OperationInProgressError, ()),
            (FrameTooLargeError, (42,)),
            (AuthFailedError, ()),
            (SnapshotTruncatedError, ("test",)),
        ]
        for cls, args in classes_and_args:
            e = cls(*args)
            assert e.code not in codes, f"Duplicate code {e.code} in {cls.__name__} and {codes[e.code]}"
            codes[e.code] = cls.__name__
