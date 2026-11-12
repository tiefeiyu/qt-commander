"""Launch an app with the runtime preloaded, attach, and verify the UI.

    python scripts/launch_e2e.py \
        --program build/injected/qt-injected-test \
        --library build/linked/qt-commander/libqt_commander_inject.dylib

Used by CI on Linux and macOS. It exercises the real MCP tool path
(``server.qt_launch``), so a failure means the tool itself is broken, not just
the plumbing.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from qt_commander import ops, server  # noqa: E402


def find_node(tree, object_name: str):
    for node in tree:
        if node.get("objectName") == object_name:
            return node
        found = find_node(node.get("children", []), object_name)
        if found:
            return found
    return None


async def _wait_for_node(session, object_name: str, timeout_s: float = 10.0):
    """Poll the live tree until ``object_name`` appears (or time out)."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout_s
    while True:
        found = await ops.query(session, where={"object_name": object_name})
        node = find_node(found.get("tree", []), object_name)
        if node is not None:
            return node
        if loop.time() >= deadline:
            return None
        await asyncio.sleep(0.2)


async def _click_by_name(session, object_name: str, **kwargs):
    node = await _wait_for_node(session, object_name)
    if node is None:
        return None
    result = await ops.act(session, node["id"], "click", **kwargs)
    if result.get("ok") is False:
        return None
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True,
                        help="executable or macOS .app bundle to launch")
    parser.add_argument("--library", required=True,
                        help="injection library to preload")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("app_args", nargs="*",
                        help="arguments passed to the application")
    return parser.parse_args()


async def main() -> int:
    args = parse_args()
    launched = await server.qt_launch(
        args.program,
        args=args.app_args or None,
        library=args.library,
        timeout_ms=args.timeout_ms,
    )
    summary = {
        key: launched.get(key)
        for key in ("ok", "session_id", "pid")
        if launched.get(key) is not None
    }
    print("launch:", json.dumps(summary, ensure_ascii=False))
    if not launched.get("ok"):
        print(json.dumps(launched.get("error"), indent=2), file=sys.stderr)
        return 1

    session_id = launched["session_id"]
    pid = (launched.get("launched") or {}).get("pid")
    session = server.sessions.get(session_id)
    try:
        found = await ops.query(
            session, where={"object_name": "injectedBtn"}, props=["text"]
        )
        node = find_node(found.get("tree", []), "injectedBtn")
        if node is None:
            print("injectedBtn not found", file=sys.stderr)
            return 1
        print("query:", node["class"], node["props"])

        clicked = await ops.act(session, node["id"], "click")
        if clicked.get("ok") is False:
            print(json.dumps(clicked, indent=2), file=sys.stderr)
            return 1

        status = await ops.query(
            session, where={"object_name": "statusLabel"}, props=["text"]
        )
        status_node = find_node(status.get("tree", []), "statusLabel")
        if status_node is None:
            print("statusLabel not found", file=sys.stderr)
            return 1
        if status_node["props"]["text"] != "clicked":
            print("click did not take effect:", status_node["props"],
                  file=sys.stderr)
            return 1
        print("verified: statusLabel =", status_node["props"]["text"])

        # Regression: an action that destroys its own target. Clicking "Close"
        # deletes the dialog (and the button) while the click is still being
        # processed; the post-action observation used to dereference the freed
        # widget and take the target process down with it.
        opened = await _click_by_name(session, "openDialogBtn")
        if opened is None:
            print("openDialogBtn not found", file=sys.stderr)
            return 1
        close_node = await _wait_for_node(session, "closeBtn", timeout_s=10)
        if close_node is None:
            print("closeBtn did not appear", file=sys.stderr)
            return 1
        # settle_ms lets the deferred deletion run before the runtime observes
        # the target, which is exactly the state that used to crash.
        closed = await ops.act(session, close_node["id"], "click",
                               settle_ms=100)
        if closed.get("ok") is False:
            print("close click failed:", json.dumps(closed), file=sys.stderr)
            return 1
        observed = closed.get("observed") or {}
        if not observed.get("gone"):
            print("expected observed.gone for a destroyed target:",
                  json.dumps(observed), file=sys.stderr)
            return 1

        # The process must still be alive and serving: query again and check
        # that the dialog is gone.
        if await _wait_for_node(session, "closeBtn", timeout_s=5) is not None:
            print("dialog survived its own destruction", file=sys.stderr)
            return 1
        status = await ops.query(
            session, where={"object_name": "statusLabel"}, props=["text"]
        )
        status_node = find_node(status.get("tree", []), "statusLabel")
        if status_node is None or status_node["props"]["text"] != "dialog closed":
            print("target did not survive the self-destroying click:",
                  status_node and status_node["props"], file=sys.stderr)
            return 1
        print("verified: self-destroying click did not crash the target")

        frame = await ops.screenshot(session, max_width=480)
        if not frame.binary.startswith(b"\x89PNG"):
            print("screenshot is not a PNG", file=sys.stderr)
            return 1
        print("screenshot:", len(frame.binary), "bytes")
    finally:
        await server.qt_detach(session_id)
        if pid:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
