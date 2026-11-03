"""Keep the agent-facing MCP instructions and tool descriptions in sync."""

from __future__ import annotations

import pytest

from qt_commander import server

EXPECTED_TOOLS = {
    "qt_status",
    "qt_windows",
    "qt_environments",
    "qt_build",
    "qt_attach",
    "qt_detach",
    "qt_query",
    "qt_items",
    "qt_act",
    "qt_batch",
    "qt_wait",
    "qt_screenshot",
    "qt_get",
    "qt_set",
    "qt_call",
    "qt_methods",
    "qt_source",
}


@pytest.mark.asyncio
async def test_mcp_instructions_cover_the_operating_loop():
    instructions = server.mcp.instructions or ""
    assert "When to use" in instructions
    assert "How to use" in instructions
    assert "Not for" in instructions
    assert "linked descriptor" in instructions
    assert "mode=\"injection\"" in instructions
    assert "qt_query" in instructions
    assert "qt_attach" in instructions
    assert "qt_detach" in instructions
    assert "E_AMBIGUOUS" in instructions
    assert "E_HIDDEN" in instructions
    assert "include_hidden" in instructions
    assert "qt_methods" in instructions
    assert "property setters" in " ".join(instructions.split())


@pytest.mark.asyncio
async def test_tool_descriptions_match_current_features():
    tools = {tool.name: tool for tool in await server.mcp.list_tools()}
    assert set(tools) == EXPECTED_TOOLS
    for tool in tools.values():
        assert tool.description and tool.description.strip()

    assert "set_geometry" in tools["qt_act"].description
    assert '"set":' in tools["qt_batch"].description
    assert '"get":' in tools["qt_batch"].description
    assert "Q_INVOKABLE" in tools["qt_call"].description
    assert "source_dir" in tools["qt_source"].description
    assert "E_NO_DEBUGINFO" in tools["qt_source"].description
    assert "pending" not in tools["qt_source"].description.lower()
