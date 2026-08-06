"""Custom error types mapped to MCP error codes (-32000 + data.code sub-codes)."""

import json


class QtCommanderError(Exception):
    """Base exception. data.code 1001-2010 maps to MCP JSON-RPC -32000."""

    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message
        super().__init__(message)


class RpcError(QtCommanderError):
    """A JSON-RPC error response from the injected library.

    Carries the library's own code + message (e.g. -32601 method not
    found, 2009 auth failed, 2004 main-thread timeout) so callers can
    distinguish failure modes instead of everything collapsing into a
    timeout.
    """

    def __init__(self, code: int, message: str):
        super().__init__(code, message)


# Element errors (1001-1005)
class ElementDestroyedError(QtCommanderError):
    def __init__(self, element_id: int):
        super().__init__(1001, f"element #{element_id} no longer exists")


class ElementStaleError(QtCommanderError):
    def __init__(self, element_id: int):
        super().__init__(1002, f"element #{element_id} is stale — call qt_snapshot first")


class ElementNotVisibleError(QtCommanderError):
    def __init__(self, element_id: int):
        super().__init__(1003, f"element #{element_id} is not visible")


class ElementDisabledError(QtCommanderError):
    def __init__(self, element_id: int):
        super().__init__(1004, f"element #{element_id} is disabled")


class ElementZeroSizeError(QtCommanderError):
    def __init__(self, element_id: int):
        super().__init__(1005, f"element #{element_id} has zero size")


# System errors (2001-2010)
class BuildRequiredError(QtCommanderError):
    def __init__(self):
        super().__init__(
            2001,
            "Build required — compile the injector and library first using qt_build. "
            "Required params: vcvars_path, qt_env. "
            "Optional: vcvars_args, build_type, qt_major, generator.",
        )


class InjectionError(QtCommanderError):
    def __init__(self, detail: str):
        super().__init__(2002, f"Injection failed: {detail}")


class RpcTimeoutError(QtCommanderError):
    def __init__(self, detail: str = ""):
        super().__init__(2003, f"Target process did not respond{': ' + detail if detail else ''}")


class MainThreadUnresponsiveError(QtCommanderError):
    def __init__(self):
        super().__init__(2004, "Main thread unresponsive after 30s")


class DiskFullError(QtCommanderError):
    def __init__(self, available_mb: int, needed_mb: int):
        super().__init__(
            2005,
            f"Disk full: {available_mb} MB available, need {needed_mb} MB",
        )


class SessionExistsError(QtCommanderError):
    def __init__(self, pid: int, session_id: str):
        super().__init__(
            2006, f"Process {pid} already attached in session {session_id}"
        )


class OperationInProgressError(QtCommanderError):
    def __init__(self):
        super().__init__(2007, "operation in progress — retry")


class FrameTooLargeError(QtCommanderError):
    def __init__(self, size: int):
        super().__init__(2008, f"frame too large: {size} bytes (max 16 MB)")


class AuthFailedError(QtCommanderError):
    def __init__(self):
        super().__init__(2009, "authentication failed")


class SnapshotTruncatedError(QtCommanderError):
    def __init__(self, reason: str):
        super().__init__(2010, f"snapshot truncated: {reason}")


class SessionLostError(QtCommanderError):
    """The target process died or the RPC connection dropped.

    The session is marked disconnected (qt_list_sessions shows
    connected:false); re-attach with qt_attach to recover.
    """

    def __init__(self, detail: str = ""):
        super().__init__(2011, f"Session lost — target process exited or "
                              f"connection dropped{': ' + detail if detail else ''}")


class SessionNotFoundError(QtCommanderError):
    """Special: maps to JSON-RPC -32602 Invalid Params."""
    def __init__(self, session_id: str):
        super().__init__(-32602, f"Invalid params: unknown session_id '{session_id}'")


def tool_error(code: int, message: str) -> str:
    """Fallback error formatting when fastmcp custom_error_handler is unavailable.

    Returns a JSON-RPC error string that the MCP client interprets correctly.
    """
    return json.dumps(
        {
            "error": {"code": -32000, "message": message, "data": {"code": code}},
        }
    )
