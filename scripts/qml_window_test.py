"""Regression for a pure QML/QQuickWindow target (no QWidget top-level)."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from qt_commander import ops
from qt_commander.client import RpcClient, RpcError
from qt_commander.session import Session


def find_qml(node, qml_id):
    if node.get("qml_id") == qml_id or node.get("objectName") == qml_id:
        return node
    for child in node.get("children", []):
        found = find_qml(child, qml_id)
        if found:
            return found
    return None


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--exe",
        default=str(ROOT / "build" / "qmlwindow-mingw5" / "qmlwindow-test.exe"),
    )
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    if not exe.exists():
        raise SystemExit(f"missing test app: {exe}")

    rendezvous = Path(tempfile.mkdtemp(prefix="qtc-qmlwindow-"))
    env = dict(os.environ)
    env["QT_COMMANDER_RENDEZVOUS"] = str(rendezvous)
    env.setdefault("QML2_IMPORT_PATH", r"C:\Software\Qt\5.15.2\mingw81_64\qml")
    process = subprocess.Popen(
        [str(exe)],
        cwd=str(exe.parent),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        descriptor = None
        for _ in range(100):
            files = list(rendezvous.glob(f"descriptor-{process.pid}.json"))
            if files:
                descriptor = json.loads(files[-1].read_text())
                break
            if process.poll() is not None:
                raise RuntimeError(f"QML test app exited: {process.returncode}")
            await asyncio.sleep(0.2)
        if descriptor is None:
            raise RuntimeError("QML test app did not publish a descriptor")

        client = await RpcClient.connect(
            "127.0.0.1", int(descriptor["port"]), descriptor["token"]
        )
        session = Session(
            descriptor["session_id"],
            int(descriptor["pid"]),
            client,
            client.hello,
        )
        windows = await session.call("windows", {"visible": True})
        assert windows["count"] == 1, windows
        window = windows["windows"][0]
        assert window["class"] == "QQuickWindow", window

        tree = await session.call(
            "query",
            {"scope": {"id": window["id"]}, "limit": 500, "props": ["text"]},
        )
        assert tree["count"] >= 5, tree
        action = find_qml(tree["tree"][0], "actionMouse")
        status = find_qml(tree["tree"][0], "statusLabel")
        field = find_qml(tree["tree"][0], "nameField")
        assert action and status and field

        items = await session.call("items", {"owner": window["id"], "limit": 20})
        assert items["visible"] >= 1, items

        got = await session.call(
            "get", {"target": {"id": action["id"]}, "props": ["objectName"]}
        )
        assert got["props"]["objectName"] == "actionMouse", got

        clicked = await session.call(
            "act", {"target": {"id": action["id"]}, "action": "click"}
        )
        assert clicked["ok"] and clicked["dispatched"], clicked
        await asyncio.sleep(0.2)
        status_after = await session.call(
            "get", {"target": {"id": status["id"]}, "props": ["text"]}
        )
        assert status_after["props"]["text"] == "clicked", status_after

        typed = await session.call(
            "act",
            {
                "target": {"id": field["id"]},
                "action": "type",
                "text": "hello",
                "clear": True,
            },
        )
        assert typed.get("text_after") == "hello", typed
        typed_get = await session.call(
            "get", {"target": {"id": field["id"]}, "props": ["text"]}
        )
        assert typed_get["props"]["text"] == "hello", typed_get

        hidden_tree = await session.call(
            "query",
            {
                "where": {"qml_id": "hiddenField"},
                "include_hidden": True,
                "limit": 10,
            },
        )
        hidden = find_qml(hidden_tree["tree"][0], "hiddenField")
        assert hidden, hidden_tree
        hidden_id = hidden["id"]
        try:
            await session.call(
                "act",
                {
                    "target": {"id": hidden_id},
                    "action": "type",
                    "text": "nope",
                },
            )
        except RpcError as exc:
            assert exc.code == "E_HIDDEN", exc.code
        else:
            raise AssertionError("hidden type unexpectedly succeeded")
        hidden_typed = await session.call(
            "act",
            {
                "target": {"id": hidden_id},
                "action": "type",
                "text": "hidden-ok",
                "clear": True,
                "include_hidden": True,
            },
        )
        assert hidden_typed.get("text_after") == "hidden-ok", hidden_typed
        try:
            await session.call(
                "set",
                {"target": {"id": hidden_id}, "name": "text", "value": "nope"},
            )
        except RpcError as exc:
            assert exc.code == "E_HIDDEN", exc.code
        else:
            raise AssertionError("hidden set unexpectedly succeeded")
        hidden_set = await session.call(
            "set",
            {
                "target": {"id": hidden_id},
                "name": "text",
                "value": "set-hidden",
                "include_hidden": True,
            },
        )
        assert hidden_set.get("new") == "set-hidden", hidden_set

        shot = await ops.screenshot(
            session, max_width=800, annotate="all", max_labels=12
        )
        assert shot.result["width"] > 0 and len(shot.binary) > 0
        assert shot.result["annotate_shown"] > 0

        element = await ops.screenshot(
            session, target={"id": action["id"]}, max_width=0, annotate="none"
        )
        assert element.result["width"] > 0 and element.result["height"] > 0

        await session.close()
        print(
            "qml-window regression: OK",
            f"tree={tree['count']}",
            f"shot={shot.result['width']}x{shot.result['height']}",
            f"element={element.result['width']}x{element.result['height']}",
        )
    finally:
        process.terminate()
        try:
            process.wait(5)
        except subprocess.TimeoutExpired:
            process.kill()


if __name__ == "__main__":
    asyncio.run(main())
