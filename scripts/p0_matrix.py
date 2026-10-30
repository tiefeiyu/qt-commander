"""P0 smoke across the matrix test app modes."""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import time
from pathlib import Path

from qt_commander.client import RpcClient
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
MODES = {
    "basic": ["btnBasic", "field", "label"],
    "complex": ["tabs", "table", "tree", "combo", "slider"],
    "custom-painted": ["customPainted", "btnCustom"],
    "multi-window": ["btnMain", "btnDialog"],
    "occlusion": ["btnTop", "btnCovered"],
    "qml": ["quick"],
    "mixed": ["quick", "btnMixed"],
}


def find_descriptor(directory: Path, pid: int) -> Path | None:
    matches = sorted(directory.glob(f"descriptor-{pid}.json"))
    return matches[-1] if matches else None


async def measure(call):
    started = time.perf_counter()
    result = await call
    return (time.perf_counter() - started) * 1000, result


async def run_mode(exe: Path, mode: str, target: str) -> dict:
    directory = ROOT / ".test-workspace" / f"matrix-{mode}"
    directory.mkdir(parents=True, exist_ok=True)
    for old in directory.glob("descriptor-*.json"):
        old.unlink()
    environment = dict(os.environ)
    environment["QT_COMMANDER_RENDEZVOUS"] = str(directory)
    process = subprocess.Popen(
        [str(exe), "--mode", mode],
        cwd=str(exe.parent),
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        deadline = time.monotonic() + 15
        descriptor = None
        while time.monotonic() < deadline:
            descriptor = find_descriptor(directory, process.pid)
            if descriptor:
                break
            await asyncio.sleep(0.2)
        if descriptor is None:
            raise RuntimeError(f"{mode}: descriptor timeout")
        data = json.loads(descriptor.read_text(encoding="utf-8"))
        client = await RpcClient.connect(
            "127.0.0.1", int(data["port"]), str(data["token"])
        )
        session = Session(data["session_id"], int(data["pid"]), client,
                          client.hello)
        try:
            await session.call("query", {"where": {"object_name": target}})
            latency, found = await measure(
                session.call("query", {"where": {"object_name": target}})
            )
            count = int(found.get("count", 0))
            shot_params = {"max_width": 1024}
            if mode == "multi-window":
                shot_params["target"] = {"object_name": "btnMain"}
            shot_latency, frame = await measure(
                session.call_frame("screenshot", shot_params)
            )
            png_path = ROOT / ".test-workspace" / f"matrix-{mode}.png"
            png_path.write_bytes(frame.binary)
            return {
                "mode": mode,
                "target": target,
                "query_ms": round(latency, 2),
                "screenshot_ms": round(shot_latency, 2),
                "count": count,
                "png_bytes": len(frame.binary),
            }
        finally:
            await session.close()
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


async def main(exe: Path, label: str) -> int:
    results = []
    for mode, targets in MODES.items():
        results.append(await run_mode(exe, mode, targets[0]))
    report = {"label": label, "results": results}
    path = ROOT / "docs" / "benchmarks" / f"{label}.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    print("written:", path)
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--label", default="matrix")
    arguments = parser.parse_args()
    raise SystemExit(asyncio.run(main(arguments.exe.resolve(),
                                     arguments.label)))
