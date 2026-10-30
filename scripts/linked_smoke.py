"""End-to-end smoke test for the linked-mode server.

Start ``build/linked-mingw5/qt-linked-test.exe`` first (with a rendezvous
dir), then run:

    .venv\\Scripts\\python.exe scripts/linked_smoke.py
"""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from qt_commander import ops
from qt_commander.client import RpcClient
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
RENDEZVOUS = ROOT / ".test-workspace" / "rendezvous"


def find_node(tree, object_name: str):
    for node in tree:
        if node.get("objectName") == object_name:
            return node
        found = find_node(node.get("children", []), object_name)
        if found:
            return found
    return None


async def main(descriptor: Path | None) -> int:
    if descriptor is None:
        candidates = sorted(RENDEZVOUS.glob("descriptor-*.json"))
        if not candidates:
            raise SystemExit(f"no descriptor under {RENDEZVOUS}")
        descriptor = candidates[-1]
    data = json.loads(descriptor.read_text(encoding="utf-8"))
    token = data["token"]
    client = await RpcClient.connect("127.0.0.1", int(data["port"]), token)
    session = Session(data["session_id"], int(data["pid"]), client, client.hello)
    try:
        print("hello:", client.hello)
        result = await ops.query(
            session,
            where={"object_name": "btnA"},
            props=["text", "enabled"],
        )
        print("query count:", result.get("count"))
        node = find_node(result.get("tree", []), "btnA")
        assert node is not None, "btnA not found"
        assert node["props"]["text"] == "Alpha"
        assert node["class"] == "QPushButton"
        print("btnA:", json.dumps(node, ensure_ascii=False))

        edit = await ops.query(
            session,
            where={"class": "QLineEdit"},
            props=["text"],
        )
        edit_node = find_node(edit.get("tree", []), "nameEdit")
        assert edit_node is not None and edit_node["props"]["text"] == "initial"
        print("lineEdit:", json.dumps(edit_node, ensure_ascii=False))

        counted = await ops.query(session, count_only=True)
        print("total count:", counted["count"])
        assert counted["count"] >= 5

        clicked = await ops.act(session, node["id"], "click")
        print("click:", clicked)
        status = await ops.query(
            session,
            where={"object_name": "statusLabel"},
            props=["text"],
        )
        status_node = find_node(status.get("tree", []), "statusLabel")
        assert status_node is not None
        assert status_node["props"]["text"] == "clicked"

        typed = await ops.act(
            session, edit_node["id"], "type", text="hello", clear=True
        )
        print("type:", typed)
        after = await ops.query(
            session,
            where={"object_name": "nameEdit"},
            props=["text"],
        )
        after_node = find_node(after.get("tree", []), "nameEdit")
        assert after_node is not None
        assert after_node["props"]["text"] == "hello"

        frame = await ops.screenshot(session, max_width=480)
        print("screenshot:", frame.result, "bytes:", len(frame.binary))
        assert frame.binary.startswith(b"\x89PNG")
        out = ROOT / ".test-workspace" / "linked.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(frame.binary)
        print("screenshot written:", out)
    finally:
        await session.close()
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--descriptor", type=Path, default=None)
    args = ap.parse_args()
    raise SystemExit(asyncio.run(main(args.descriptor)))



