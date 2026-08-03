"""Cross-check RPC method names between the Python server and the library.

The library dispatches on opMethod names like "click", "focus", "typeText".
If the Python side ever sends a name the library does not dispatch (e.g.
the historical "qt.mouseClick" vs "qt.click" mismatch), every call fails
with "Method not found".  This test pins the two sides together.
"""
import re
from pathlib import Path

SERVER_PY = Path(__file__).resolve().parents[2] / "qt_commander" / "server.py"
RPC_SERVER_CPP = (
    Path(__file__).resolve().parents[2] / "src" / "library" / "rpc" / "rpc_server.cpp"
)
SESSION_PY = Path(__file__).resolve().parents[2] / "qt_commander" / "session.py"


def _python_sent_methods() -> set[str]:
    """Methods sent by the Python side, without the 'qt.' prefix."""
    methods: set[str] = set()
    for f in (SERVER_PY, SESSION_PY):
        src = f.read_text(encoding="utf-8")
        methods.update(re.findall(r'send_rpc\("qt\.([a-zA-Z]+)"', src))
    return methods


def _library_dispatched_methods() -> set[str]:
    """opMethod branch names handled by the library's RPC dispatch."""
    src = RPC_SERVER_CPP.read_text(encoding="utf-8")
    return set(re.findall(r'opMethod == QStringLiteral\("([a-zA-Z]+)"\)', src))


# These are routed outside the opMethod dispatch table.
_ROUTED_ELSEWHERE = {"authenticate", "shutdown"}


def test_python_methods_are_dispatched_by_library():
    sent = _python_sent_methods()
    dispatched = _library_dispatched_methods()
    assert sent, "no send_rpc calls found"
    assert dispatched, "no dispatch branches found"

    missing = sent - dispatched - _ROUTED_ELSEWHERE
    assert not missing, (
        f"RPC methods sent by Python are not dispatched by the library: "
        f"{sorted(missing)}"
    )


def test_authenticate_and_shutdown_have_dedicated_handling():
    """authenticate/shutdown must be handled outside the opMethod table."""
    src = RPC_SERVER_CPP.read_text(encoding="utf-8")
    assert '"qt.authenticate"' in src
    assert '"qt.shutdown"' in src
