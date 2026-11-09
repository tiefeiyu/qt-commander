"""P0 benchmark for the large linked application.

Start the app first, e.g.:

    build/large-mingw5/qt-large-test.exe --nodes 7000 --hidden 500 \
        --rows 500 --cols 20 --ready-file .test-workspace/large.ready

then run:

    .venv\\Scripts\\python.exe scripts\\p0_large.py --label large-7000
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import time
from pathlib import Path

from qt_commander import ops
from qt_commander.client import RpcClient
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
RENDEZVOUS = ROOT / ".test-workspace" / "rendezvous-large"
OUT = ROOT / "docs" / "benchmarks"


def stats(values: list[float]) -> dict:
    ordered = sorted(values)
    return {
        "n": len(ordered),
        "mean": round(statistics.mean(ordered), 2),
        "p50": round(ordered[len(ordered) // 2], 2),
        "p95": round(ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))], 2),
        "max": round(ordered[-1], 2),
    }


def find_node(tree, object_name):
    for node in tree:
        if node.get("objectName") == object_name:
            return node
        found = find_node(node.get("children", []), object_name)
        if found:
            return found
    return None


async def main(descriptor: Path | None, label: str, node: str, text: str, iterations: int) -> int:
    if descriptor is None:
        candidates = sorted(RENDEZVOUS.glob("descriptor-*.json"))
        if not candidates:
            raise SystemExit(f"no descriptor under {RENDEZVOUS}")
        descriptor = candidates[-1]
    data = json.loads(descriptor.read_text(encoding="utf-8"))
    client = await RpcClient.connect("127.0.0.1", int(data["port"]), data["token"])
    session = Session(data["session_id"], int(data["pid"]), client, client.hello)
    results: dict[str, list[float]] = {}
    sizes: dict[str, list[int]] = {}

    async def measure(key: str, coro):
        start = time.perf_counter()
        value = await coro
        results.setdefault(key, []).append((time.perf_counter() - start) * 1000)
        return value

    try:
        # Warm the GUI/font/layout caches before measuring.
        await ops.query(session, where={"object_name": node}, props=["text"])
        await ops.query(session, count_only=True)
        await ops.screenshot(session, max_width=1024)

        for _ in range(iterations):
            found = await measure(
                "query_object_name",
                ops.query(
                    session,
                    where={"object_name": node},
                    props=["text"],
                ),
            )
            sizes.setdefault("query_object_name", []).append(
                len(json.dumps(found, ensure_ascii=False))
            )
            assert found["count"] == 1

            text_hit = await measure(
                "query_text",
                ops.query(
                    session,
                    where={"text": text},
                    props=["text"],
                ),
            )
            sizes.setdefault("query_text", []).append(
                len(json.dumps(text_hit, ensure_ascii=False))
            )

            counted = await measure(
                "query_count_only",
                ops.query(session, count_only=True),
            )
            sizes.setdefault("query_count_only", []).append(
                len(json.dumps(counted, ensure_ascii=False))
            )

            frame = await measure(
                "screenshot_1024",
                ops.screenshot(session, max_width=1024),
            )
            sizes.setdefault("screenshot_1024", []).append(len(frame.binary))

        tabs = await ops.query(session, where={"object_name": "mainTabs"})
        tabs_node = find_node(tabs.get("tree", []), "mainTabs")
        assert tabs_node is not None
        await ops.set_property(session, tabs_node["id"], "currentIndex", 1)

        table = await ops.query(session, where={"object_name": "largeTable"})
        table_node = find_node(table.get("tree", []), "largeTable")
        assert table_node is not None
        for _ in range(iterations):
            listed = await measure(
                "items_table",
                ops.items(session, table_node["id"], limit=200, compact=True),
            )
            sizes.setdefault("items_table", []).append(
                len(json.dumps(listed, ensure_ascii=False))
            )

        report = {
            "label": label,
            "pid": data["pid"],
            "iterations": iterations,
            "latency": {k: stats(v) for k, v in results.items()},
            "bytes": {k: stats([float(x) for x in v]) | {"last": v[-1]}
                      for k, v in sizes.items()},
        }
        OUT.mkdir(parents=True, exist_ok=True)
        path = OUT / f"{label}.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                        encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))
        print("written:", path)
    finally:
        await session.close()
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptor", type=Path, default=None)
    parser.add_argument("--label", default="large")
    parser.add_argument("--node", default="btn_5000")
    parser.add_argument("--text", default="Button 5000")
    parser.add_argument("--iterations", type=int, default=3)
    args = parser.parse_args()
    raise SystemExit(asyncio.run(main(
        args.descriptor, args.label, args.node, args.text, args.iterations
    )))
