"""Inject the bootstrap DLL into a Qt app and exercise the injected session."""
from __future__ import annotations

import argparse
import asyncio
import json

from qt_commander import ops, server


async def main(pid: int, mode: str) -> int:
    attached = await server.qt_attach(pid=pid, mode=mode)
    print("attach:", json.dumps(attached, ensure_ascii=False))
    if not attached.get("ok"):
        return 1
    session = server.sessions.get(attached["session_id"])
    try:
        found = await ops.query(
            session, where={"object_name": "injectedBtn"}
        )
        print("query:", json.dumps(found, ensure_ascii=False)[:500])
        acted = await ops.act(
            session, {"object_name": "injectedBtn"}, "click"
        )
        print("act:", json.dumps(acted, ensure_ascii=False))
        status = await ops.query(
            session, where={"object_name": "statusLabel"}, props=["text"]
        )
        print("status:", json.dumps(status, ensure_ascii=False)[:500])
    finally:
        await server.qt_detach(attached["session_id"])
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("pid", type=int)
    parser.add_argument("--mode", default="injection",
                        choices=["injection", "auto"])
    arguments = parser.parse_args()
    raise SystemExit(asyncio.run(main(arguments.pid, arguments.mode)))
