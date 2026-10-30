"""Read qt_source for a descriptor and target objectName."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from qt_commander import ops
from qt_commander.client import RpcClient
from qt_commander.session import Session


async def main(descriptor: Path, target: str) -> int:
    data = json.loads(descriptor.read_text(encoding="utf-8"))
    client = await RpcClient.connect(
        "127.0.0.1", int(data["port"]), str(data["token"])
    )
    session = Session(data["session_id"], int(data["pid"]), client,
                      client.hello)
    try:
        result = await ops.source(session, {"object_name": target})
        print(json.dumps(result, indent=2, ensure_ascii=False))
    finally:
        await session.close()
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("descriptor", type=Path)
    parser.add_argument("target")
    arguments = parser.parse_args()
    raise SystemExit(asyncio.run(main(arguments.descriptor,
                                      arguments.target)))
