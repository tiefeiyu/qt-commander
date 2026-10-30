"""Multi-window, modal, popup, dock, custom-model and QML regression."""
from __future__ import annotations

import asyncio
import json
import os
import subprocess
import time
from pathlib import Path

from qt_commander.client import RpcClient
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / "matrix-mingw5" / "qt-matrix-test.exe"
RENDEZVOUS = ROOT / ".test-workspace" / "window-regression"


async def connect(process: subprocess.Popen, directory: Path) -> Session:
    deadline = time.monotonic() + 15
    descriptor = None
    while time.monotonic() < deadline:
        files = sorted(directory.glob(f"descriptor-{process.pid}.json"))
        if files:
            descriptor = files[-1]
            break
        await asyncio.sleep(0.2)
    if descriptor is None:
        raise RuntimeError(f"descriptor timeout for {process.args}")
    data = json.loads(descriptor.read_text(encoding="utf-8"))
    client = await RpcClient.connect("127.0.0.1", int(data["port"]), data["token"])
    return Session(data["session_id"], int(data["pid"]), client, client.hello)


def find(tree, object_name):
    if tree.get("objectName") == object_name:
        return tree
    for child in tree.get("children", []):
        found = find(child, object_name)
        if found:
            return found
    return None


async def run_mode(mode: str) -> None:
    directory = RENDEZVOUS / mode
    directory.mkdir(parents=True, exist_ok=True)
    for old in directory.glob("descriptor-*.json"):
        old.unlink()
    env = dict(os.environ)
    env["QT_COMMANDER_RENDEZVOUS"] = str(directory)
    process = subprocess.Popen(
        [str(EXE), "--mode", mode],
        cwd=str(EXE.parent),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        session = await connect(process, directory)
        windows = await session.call("windows", {})
        print(mode, "windows:", [(w["objectName"], w["active"], w["modal"])
                                 for w in windows["windows"]])
        if mode == "multi-window":
            assert windows["count"] == 3
            await session.call("query", {"where": {"object_name": "btnIndependent"}})
            frame = await session.call_frame("screenshot", {
                "target": {"object_name": "independent"},
                "annotate": "all",
            })
            assert frame.result["legend"]
            independent_button = frame.result["legend"][0]
            acted = await session.call("act", {
                "target": {"id": independent_button["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            print(mode, "independent screenshot/act:", frame.result["width"],
                  frame.result["height"])
            try:
                await session.call("screenshot", {})
            except Exception as exc:
                print(mode, "default screenshot rejected:", type(exc).__name__)
            else:
                raise AssertionError("default screenshot should be ambiguous")
        elif mode == "multi-mainwindow":
            assert windows["count"] == 2
            assert all(w["class"] == "QMainWindow"
                       for w in windows["windows"])
            second = next(w for w in windows["windows"]
                          if w["objectName"] == "windowB")
            scoped = await session.call("query", {
                "scope": {"id": second["id"]},
                "props": ["text"],
            })
            assert len(scoped["tree"]) == 1
            assert scoped["tree"][0]["class"] == "QMainWindow"
            alias = await session.call("query", {
                "where": {"objectName": "btnMainA"},
            })
            assert alias["count"] == 1
            frame = await session.call_frame("screenshot", {
                "target": {"id": second["id"]},
                "annotate": "all",
            })
            assert frame.result["legend"]
            button = frame.result["legend"][0]
            acted = await session.call("act", {
                "target": {"id": button["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            try:
                await session.call("screenshot", {})
            except Exception as exc:
                print(mode, "default screenshot rejected:", type(exc).__name__)
            else:
                raise AssertionError("default screenshot should be ambiguous")
            minimized = await session.call("call", {
                "target": {"id": second["id"]},
                "method": "showMinimized",
            })
            assert minimized["ok"] is True
            await asyncio.sleep(0.2)
            minimized_windows = await session.call("windows", {})
            minimized_second = next(
                w for w in minimized_windows["windows"]
                if w["objectName"] == "windowB"
            )
            assert minimized_second["minimized"] is True
            try:
                await session.call_frame("screenshot", {
                    "target": {"id": second["id"]},
                })
            except Exception as exc:
                print(mode, "minimized screenshot rejected:",
                      type(exc).__name__)
            else:
                raise AssertionError("minimized screenshot should fail")
            try:
                await session.call("act", {
                    "target": {"id": button["id"]},
                    "action": "click",
                })
            except Exception as exc:
                print(mode, "minimized act rejected:", type(exc).__name__)
            else:
                raise AssertionError("minimized act should fail")
            restored = await session.call("call", {
                "target": {"id": second["id"]},
                "method": "showNormal",
            })
            assert restored["ok"] is True
            print(mode, "second window screenshot/act:", frame.result["width"],
                  frame.result["height"])
        elif mode == "occlusion":
            frame = await session.call_frame("screenshot", {
                "target": {"id": windows["windows"][0]["id"]},
                "annotate": "all",
            })
            assert len(frame.result["legend"]) >= 2
            covered = await session.call("query", {
                "where": {"object_name": "btnCovered"},
            })
            button = find(covered["tree"][0], "btnCovered")
            acted = await session.call("act", {
                "target": {"id": button["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            print(mode, "covered act:", button["id"])
        elif mode == "modal":
            await asyncio.sleep(0.5)
            windows = await session.call("windows", {})
            modal = next(w for w in windows["windows"]
                         if w["objectName"] == "modalDialog")
            assert modal["modal"] is True
            frame = await session.call_frame("screenshot", {
                "target": {"id": modal["id"]},
                "annotate": "all",
            })
            assert frame.result["legend"]
            modal_tree = await session.call("query", {
                "where": {"object_name": "btnCloseModal"},
            })
            close = find(modal_tree["tree"][0], "btnCloseModal")
            acted = await session.call("act", {
                "target": {"id": close["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            print(mode, "closed modal:", modal["id"])
        elif mode == "popup":
            await asyncio.sleep(0.5)
            windows = await session.call("windows", {})
            popup = next(w for w in windows["windows"]
                         if w["objectName"] == "popupMenu")
            assert popup["popup"] is True
            frame = await session.call_frame("screenshot", {
                "target": {"id": popup["id"]},
                "annotate": "all",
            })
            assert frame.result["width"] > 0
            assert len(frame.result["legend"]) >= 1
            print(mode, "popup legend:", frame.result["legend"][0]["id"],
                  frame.result["legend"][0]["text"])
            items = await session.call("items", {"owner": popup["id"]})
            print(mode, "popup items:", json.dumps(items, ensure_ascii=False)[:1000])
            action = next(item for item in items["items"]
                          if item["props"].get("text") == "Popup action")
            acted = await session.call("act", {
                "target": {"id": action["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            status = await session.call("query", {
                "where": {"object_name": "popupStatus"},
                "props": ["text"],
            })
            text = find(status["tree"][0], "popupStatus")["props"]["text"]
            print(mode, "popup result:", text)
        elif mode == "dock":
            dock_tree = await session.call("query", {
                "where": {"object_name": "dock"},
            })
            dock = find(dock_tree["tree"][0], "dock")
            items = await session.call("items", {"owner": dock["id"]})
            assert len(items["items"]) >= 2
            acted = await session.call("act", {
                "target": {"id": items["items"][-1]["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            toolbar_tree = await session.call("query", {
                "where": {"object_name": "toolbar"},
            })
            toolbar = find(toolbar_tree["tree"][0], "toolbar")
            toolbar_items = await session.call("items", {"owner": toolbar["id"]})
            assert toolbar_items["items"]
            toolbar_act = await session.call("act", {
                "target": {"id": toolbar_items["items"][0]["id"]},
                "action": "click",
            })
            assert toolbar_act["ok"] is True
            status_tree = await session.call("query", {
                "where": {"object_name": "dockStatus"},
                "props": ["text"],
            })
            assert find(status_tree["tree"][0], "dockStatus")["props"]["text"] == "triggered"
            print(mode, "dock buttons:", len(items["items"]),
                  "toolbar actions:", len(toolbar_items["items"]))
        elif mode == "custom-model":
            table_tree = await session.call("query", {
                "where": {"object_name": "customTable"},
            })
            table = find(table_tree["tree"][0], "customTable")
            cells = await session.call("items", {"owner": table["id"]})
            assert cells["items"]
            assert cells["items"][0]["props"]["text"] == "m00"
            splitter_tree = await session.call("query", {
                "where": {"object_name": "splitter"},
            })
            splitter = find(splitter_tree["tree"][0], "splitter")
            handles = await session.call("items", {"owner": splitter["id"]})
            assert handles["items"]
            print(mode, "custom cells:", len(cells["items"]),
                  "splitter handles:", len(handles["items"]))
        elif mode == "qml":
            text_tree = await session.call("query", {
                "where": {"object_name": "qmlText"},
                "props": ["text"],
            })
            text_node = find(text_tree["tree"][0], "qmlText")
            assert text_node["qml_id"] == "qmlText"
            mouse_tree = await session.call("query", {
                "where": {"object_name": "qmlMouse"},
            })
            mouse = find(mouse_tree["tree"][0], "qmlMouse")
            acted = await session.call("act", {
                "target": {"id": mouse["id"]},
                "action": "click",
            })
            assert acted["ok"] is True
            status = await session.call("query", {
                "where": {"object_name": "qmlStatus"},
                "props": ["text"],
            })
            assert find(status["tree"][0], "qmlStatus")["props"]["text"] == "clicked"
            quick_tree = await session.call("query", {
                "where": {"object_name": "quick"},
            })
            quick = find(quick_tree["tree"][0], "quick")
            frame = await session.call_frame("screenshot", {
                "target": {"id": quick["id"]},
                "annotate": "all",
            })
            assert any(entry.get("qml_id") == "qmlText"
                       for entry in frame.result["legend"])
            source = await session.call("source", {"target": text_node["id"]})
            assert source["available"] is True
            assert source["kind"] == "qml"
            print(mode, "qml legend:", len(frame.result["legend"]),
                  "source line:", source["location"]["line"])
        await session.close()
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


async def main() -> None:
    for mode in ("multi-window", "multi-mainwindow", "occlusion", "modal",
                 "popup", "dock", "custom-model", "qml"):
        await run_mode(mode)


asyncio.run(main())
