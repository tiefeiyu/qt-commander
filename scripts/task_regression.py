"""Async task-queue regression for the target-side scheduler.

Covers: fast act, pending + task_id, queue availability while a task runs,
qt_task collection, nested exec(), WA_DeleteOnClose + settle, batch semantics
on a pending step, server-side wait_ms defaults and a short soak.

Build the target once:

    cmake -S tests/test-apps/modal -B build/modal-mingw5 -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DQT_MAJOR_VERSION=5 \
          -DCMAKE_PREFIX_PATH=<Qt5 kit>

Example:

    .venv\\Scripts\\python.exe scripts/task_regression.py
    .venv\\Scripts\\python.exe scripts/task_regression.py --exe path/to/qt-modal-test.exe
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from qt_commander import ops
from qt_commander.client import RpcClient, RpcError
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / "build" / "modal-mingw5" / "qt-modal-test.exe"


def find(tree, object_name):
    if tree.get("objectName") == object_name:
        return tree
    for child in tree.get("children", []):
        hit = find(child, object_name)
        if hit:
            return hit
    return None


async def connect(process: subprocess.Popen, directory: Path) -> Session:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        files = sorted(directory.glob(f"descriptor-{process.pid}.json"))
        if files:
            data = json.loads(files[-1].read_text(encoding="utf-8"))
            client = await RpcClient.connect(
                "127.0.0.1", int(data["port"]), data["token"]
            )
            return Session(data["session_id"], int(data["pid"]), client,
                           client.hello)
        await asyncio.sleep(0.1)
    raise RuntimeError(f"descriptor timeout for {process.args}")


async def node_id(session: Session, name: str) -> str:
    result = await session.call(
        "query", {"where": {"object_name": name}, "include_hidden": True}
    )
    node = find(result["tree"][0], name)
    if node is None:
        raise AssertionError(f"missing node {name}")
    return node["id"]


async def wait_task(session: Session, task_id: str, timeout_ms: int = 3000):
    return await ops.task(session, task_id, wait_ms=timeout_ms)


async def scenario_fast(session: Session, process: subprocess.Popen,
                        label: str) -> None:
    fast = await ops.act(session, {"object_name": "btnFast"}, "click")
    assert fast.get("ok") and fast.get("state") is None, fast

    open_id = await node_id(session, "btnOpenModal")
    started = time.monotonic()
    pending = await ops.act(session, {"id": open_id}, "click")
    elapsed = (time.monotonic() - started) * 1000
    assert pending.get("state") == "running", pending
    assert pending.get("task_id"), pending
    assert 800 <= elapsed <= 3000, elapsed

    pong = await session.call("ping", {}, timeout=2.0)
    assert pong.get("inflight", 0) >= 1, pong

    close_id = await node_id(session, "btnCloseDialog")
    closed = await ops.act(session, {"id": close_id}, "click")
    assert closed.get("ok"), closed

    done = await wait_task(session, pending["task_id"])
    assert done.get("state") == "done", done
    assert (done.get("result") or {}).get("ok") is True, done

    try:
        await ops.task(session, "no-such-task")
    except RpcError as exc:
        assert exc.code == "E_TASK_NOT_FOUND", exc
    else:
        raise AssertionError("unknown task must fail")
    print(f"{label} click-modal: pending {elapsed:.0f} ms -> done")


async def scenario_nested(session: Session, process: subprocess.Popen,
                          label: str) -> None:
    a_id = await node_id(session, "btnOpenA")
    first = await ops.act(session, {"id": a_id}, "click")
    assert first.get("state") == "running", first

    b_id = await node_id(session, "btnOpenB")
    second = await ops.act(session, {"id": b_id}, "click")
    assert second.get("state") == "running", second

    pong = await session.call("ping", {}, timeout=2.0)
    assert pong.get("inflight") == 2, pong

    for name, task in (("btnCloseB", second), ("btnCloseA", first)):
        target = await node_id(session, name)
        acted = await ops.act(session, {"id": target}, "click")
        assert acted.get("ok"), acted

    for task in (first, second):
        done = await wait_task(session, task["task_id"])
        assert done.get("state") == "done", done
    print(f"{label} nested-modal: depth 2, both tasks done")


async def scenario_delete(session: Session, process: subprocess.Popen,
                          label: str) -> None:
    open_id = await node_id(session, "btnOpenDoomed")
    pending = await ops.act(session, {"id": open_id}, "click")
    assert pending.get("state") == "running", pending

    close_id = await node_id(session, "btnCloseDoomed")
    closed = await ops.act(session, {"id": close_id}, "click", settle_ms=500)
    assert closed.get("ok"), closed

    pong = await session.call("ping", {}, timeout=2.0)
    assert pong.get("pong") is True
    assert process.poll() is None, "target crashed after WA_DeleteOnClose"
    done = await wait_task(session, pending["task_id"])
    assert done.get("state") == "done", done
    print(f"{label} delete-on-close: survived, task done")


async def scenario_batch(session: Session, process: subprocess.Popen,
                         label: str) -> None:
    open_id = await node_id(session, "btnOpenModal")
    close_id = await node_id(session, "btnCloseDialog")
    result = await ops.batch(
        session,
        [
            {"act": {"target": {"id": open_id}, "action": "click",
                     "wait_ms": 500}},
            {"act": {"target": {"id": close_id}, "action": "click",
                     "wait_ms": 500}},
        ],
        timeout_ms=30000,
    )
    assert result.get("ok") is False, result
    assert result.get("cause") == "pending", result
    task_id = result.get("task_id")
    assert task_id, result
    assert len(result["steps"]) == 1, result

    await ops.act(session, {"id": close_id}, "click")
    done = await wait_task(session, task_id)
    assert done.get("state") == "done", done
    print(f"{label} batch: stopped with cause=pending, task {task_id} done")


async def scenario_default_wait(session: Session, process: subprocess.Popen,
                               label: str) -> None:
    open_id = await node_id(session, "btnOpenModal")
    started = time.monotonic()
    pending = await session.call(
        "act", {"target": {"id": open_id}, "action": "click"}, timeout=10
    )
    elapsed = (time.monotonic() - started) * 1000
    assert pending.get("state") == "running", pending
    assert 800 <= elapsed <= 3000, elapsed
    close_id = await node_id(session, "btnCloseDialog")
    await ops.act(session, {"id": close_id}, "click")
    done = await wait_task(session, pending["task_id"])
    assert done.get("state") == "done", done
    print(f"{label} default wait_ms: {elapsed:.0f} ms -> done")


async def scenario_soak(session: Session, process: subprocess.Popen,
                        label: str, cycles: int = 8) -> None:
    open_id = await node_id(session, "btnOpenModal")
    close_id = await node_id(session, "btnCloseDialog")
    started = time.monotonic()
    for _ in range(cycles):
        pending = await ops.act(session, {"id": open_id}, "click",
                                wait_ms=150)
        assert pending.get("state") == "running", pending
        closed = await ops.act(session, {"id": close_id}, "click",
                               wait_ms=150)
        assert closed.get("ok"), closed
        done = await wait_task(session, pending["task_id"], 3000)
        assert done.get("state") == "done", done
    pong = await session.call("ping", {}, timeout=2.0)
    assert pong.get("inflight") == 0, pong
    assert pong.get("queued") == 0, pong
    elapsed = (time.monotonic() - started) * 1000
    print(f"{label} soak: {cycles} cycles in {elapsed:.0f} ms, inflight 0")


async def run_mode(exe: Path, mode: str, scenario, label: str) -> None:
    directory = Path(tempfile.mkdtemp(prefix=f"qtc-task-{mode}-"))
    env = dict(os.environ)
    env["QT_COMMANDER_RENDEZVOUS"] = str(directory)
    process = subprocess.Popen(
        [str(exe), "--mode", mode],
        cwd=str(exe.parent),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    session = None
    try:
        session = await connect(process, directory)
        await scenario(session, process, label)
    finally:
        if session is not None:
            await session.close()
        process.terminate()
        process.wait(timeout=5)


async def main(exe: Path, label: str) -> int:
    if not exe.is_file():
        raise SystemExit(f"target not built: {exe}")
    await run_mode(exe, "click-modal", scenario_fast, label)
    await run_mode(exe, "nested-modal", scenario_nested, label)
    await run_mode(exe, "delete-on-close", scenario_delete, label)
    await run_mode(exe, "click-modal", scenario_batch, label)
    await run_mode(exe, "click-modal", scenario_default_wait, label)
    await run_mode(exe, "click-modal", scenario_soak, label)
    print("task regression: OK")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--label", default="task")
    arguments = parser.parse_args()
    raise SystemExit(asyncio.run(main(arguments.exe.resolve(),
                                      arguments.label)))

