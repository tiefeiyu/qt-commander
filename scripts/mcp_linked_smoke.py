"""MCP stdio smoke test for the server (linked mode)."""
from __future__ import annotations

import asyncio
import base64
import json
import os
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
RENDEZVOUS = Path(os.environ.get(
    "QT_COMMANDER_RENDEZVOUS",
    str(ROOT / ".test-workspace" / "rendezvous"),
))


def find_node(tree, object_name):
    for node in tree:
        if node.get("objectName") == object_name:
            return node
        found = find_node(node.get("children", []), object_name)
        if found:
            return found
    return None


def first_json(result):
    for block in result.content:
        text = getattr(block, "text", None)
        if text is not None:
            return json.loads(text)
    return None


async def main() -> int:
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "qt_commander.server"],
        cwd=str(ROOT),
    )
    candidates = sorted(RENDEZVOUS.glob("descriptor-*.json"))
    if not candidates:
        raise SystemExit(f"no descriptor under {RENDEZVOUS}")

    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            print("tools:", len(tools.tools))
            print(", ".join(tool.name for tool in tools.tools))

            attached = await session.call_tool(
                "qt_attach", {"descriptor": str(candidates[-1])}
            )
            info = first_json(attached)
            print("attach:", info)
            assert info and info.get("ok") is True
            sid = info["session_id"]

            found = first_json(await session.call_tool("qt_query", {
                "session_id": sid,
                "where": {"object_name": "btnA"},
                "props": ["text"],
            }))
            print("query:", found)

            ids = {}
            for name in ("mainTabs", "table", "tableHeader", "slider",
                         "menuBar", "tree", "list", "scroll", "combo"):
                result = first_json(await session.call_tool("qt_query", {
                    "session_id": sid,
                    "where": {"object_name": name},
                }))
                node = find_node(result.get("tree", []), name)
                assert node is not None, name
                ids[name] = node["id"]

            listed_by_name = {}
            for name, expected in (
                ("mainTabs", 3),
                ("table", 1),
                ("tableHeader", 2),
                ("slider", 1),
                ("menuBar", 1),
                ("tree", 2),
                ("list", 2),
                ("scroll", 1),
            ):
                listed = first_json(await session.call_tool("qt_items", {
                    "owner": ids[name],
                    "limit": 20,
                }))
                print(f"items[{name}]:", listed)
                assert listed["total"] >= expected, name
                assert len(listed["items"]) >= expected, name
                listed_by_name[name] = listed

            combo_list = first_json(await session.call_tool("qt_items", {
                "owner": ids["combo"],
                "limit": 10,
            }))
            print("items[combo]:", combo_list)
            assert combo_list["total"] >= 3
            combo_id = combo_list["items"][1]["id"]
            combo_get = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": combo_id,
            }))
            assert combo_get["virtual"] is True
            combo_click = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": combo_id,
                "action": "click",
            }))
            print("combo item act:", combo_click)
            assert combo_click.get("ok") is True

            got = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": found["tree"][0]["children"][0]["children"][0]["id"],
                "props": ["text", "enabled"],
            }))
            print("get:", got)
            expected = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "btnA"},
                "action": "click",
                "expect": {
                    "where": {"object_name": "statusLabel"},
                    "props": {"text": "clicked"},
                },
            }))
            print("expect act:", expected)
            assert expected.get("ok") is True
            assert expected["expect"]["matched"] is True

            wide = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": {"object_name": "btnA"},
                "props": "*",
            }))
            print("wildcard props:", len(wide.get("props", {})))
            assert len(wide.get("props", {})) > 5

            ambiguous = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": {"object_name": "twinBtn"},
            }))
            print("ambiguous get:", ambiguous)
            assert ambiguous["error"]["code"] == "E_AMBIGUOUS"

            nth = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "twinBtn"},
                "action": "click",
                "nth": 1,
            }))
            print("nth act:", nth)
            assert nth.get("ok") is True

            source = first_json(await session.call_tool("qt_source", {
                "session_id": sid,
                "target": found["tree"][0]["children"][0]["children"][0]["id"],
            }))
            print("source:", source)
            assert source["available"] is False
            assert source["code"] == "E_NO_DEBUGINFO"

            status = first_json(await session.call_tool("qt_query", {
                "session_id": sid,
                "where": {"object_name": "statusLabel"},
                "props": ["text"],
            }))
            status_id = status["tree"][0]["children"][0]["children"][0]["id"]

            set_result = first_json(await session.call_tool("qt_set", {
                "session_id": sid,
                "target": status_id,
                "name": "text",
                "value": "set-by-rpc",
            }))
            print("set:", set_result)

            clicked = first_json(await session.call_tool("qt_call", {
                "session_id": sid,
                "target": found["tree"][0]["children"][0]["children"][0]["id"],
                "method": "click",
            }))
            print("call click:", clicked)

            status_after = first_json(await session.call_tool("qt_query", {
                "session_id": sid,
                "where": {"object_name": "statusLabel"},
                "props": ["text"],
            }))
            print("status:", status_after)

            cleared = first_json(await session.call_tool("qt_call", {
                "session_id": sid,
                "target": {"object_name": "nameEdit"},
                "method": "clear",
            }))
            print("call clear:", cleared)

            typed = first_json(await session.call_tool("qt_call", {
                "session_id": sid,
                "target": {"object_name": "nameEdit"},
                "method": "setText",
                "args": ["typed-by-call"],
            }))
            print("call setText:", typed)
            assert typed.get("ok") is True
            typed_query = first_json(await session.call_tool("qt_query", {
                "session_id": sid,
                "where": {"object_name": "nameEdit"},
                "props": ["text"],
            }))
            typed_node = find_node(typed_query.get("tree", []), "nameEdit")
            assert typed_node is not None
            assert typed_node["props"]["text"] == "typed-by-call"

            keyed = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "nameEdit"},
                "action": "key",
                "keys": ["A", "B"],
            }))
            print("key act:", keyed)
            assert keyed.get("ok") is True
            keyed_query = first_json(await session.call_tool("qt_query", {
                "session_id": sid,
                "where": {"object_name": "nameEdit"},
                "props": ["text"],
            }))
            keyed_node = find_node(keyed_query.get("tree", []), "nameEdit")
            assert keyed_node["props"]["text"].endswith("AB")

            scrolled = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "scroll"},
                "action": "scroll",
                "dx": 0,
                "dy": -120,
            }))
            print("scroll act:", scrolled)
            assert scrolled.get("ok") is True
            hovered = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "btnA"},
                "action": "hover",
            }))
            print("hover act:", hovered)
            assert hovered.get("ok") is True
            dragged = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": {"object_name": "scroll"},
                "action": "drag",
                "to": {"x": 150, "y": 5},
            }))
            print("drag act:", dragged)
            assert dragged.get("ok") is True

            deleted = first_json(await session.call_tool("qt_call", {
                "session_id": sid,
                "target": {"object_name": "btnDelete"},
                "method": "click",
            }))
            print("delete alpha:", deleted)
            await asyncio.sleep(0.2)
            stale = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": found["tree"][0]["children"][0]["children"][0]["id"],
            }))
            print("stale get:", stale)
            assert stale.get("ok") is False

            tab_id = listed_by_name["mainTabs"]["items"][1]["id"]
            virtual = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": tab_id,
            }))
            print("virtual get:", virtual)
            assert virtual.get("virtual") is True
            tab_click = first_json(await session.call_tool("qt_act", {
                "session_id": sid,
                "target": tab_id,
                "action": "click",
            }))
            print("virtual act:", tab_click)
            assert tab_click.get("ok") is True
            virtual_shot = await session.call_tool("qt_screenshot", {
                "session_id": sid,
                "target": tab_id,
                "max_width": 120,
            })
            virtual_meta = None
            for block in virtual_shot.content:
                data = getattr(block, "data", None)
                if data:
                    print("virtual screenshot bytes:", len(base64.b64decode(data)))
                elif getattr(block, "text", None):
                    virtual_meta = json.loads(block.text)
            assert virtual_meta and virtual_meta.get("virtual") is True
            tabs_after = first_json(await session.call_tool("qt_items", {
                "owner": ids["mainTabs"],
                "limit": 20,
            }))
            assert tabs_after["items"][1]["props"]["selected"] is True
            tab_after_get = first_json(await session.call_tool("qt_get", {
                "session_id": sid,
                "target": tab_id,
            }))
            assert tab_after_get["props"]["selected"] is True

            shot = await session.call_tool("qt_screenshot", {
                "session_id": sid,
                "max_width": 480,
            })
            for block in shot.content:
                data = getattr(block, "data", None)
                if data:
                    png = base64.b64decode(data)
                    out = ROOT / ".test-workspace" / "mcp.png"
                    out.write_bytes(png)
                    print("screenshot bytes:", len(png), "->", out)

            annotated = await session.call_tool("qt_screenshot", {
                "session_id": sid,
                "max_width": 480,
                "annotate": "all",
            })
            legend = None
            for block in annotated.content:
                data = getattr(block, "data", None)
                if data:
                    png = base64.b64decode(data)
                    out = ROOT / ".test-workspace" / "mcp-annotated.png"
                    out.write_bytes(png)
                    print("annotated bytes:", len(png), "->", out)
                elif getattr(block, "text", None):
                    legend = json.loads(block.text).get("legend")
            print("legend entries:", len(legend or []))
            assert legend

            detached = first_json(await session.call_tool(
                "qt_detach", {"session_id": sid}
            ))
            print("detach:", detached)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))



