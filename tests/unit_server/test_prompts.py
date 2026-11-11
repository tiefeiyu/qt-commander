"""Keep the agent-facing MCP instructions and tool descriptions in sync."""

from __future__ import annotations

import json

import pytest

from qt_commander import server

EXPECTED_TOOLS = {
    "qt_status",
    "qt_target_info",
    "qt_processes",
    "qt_windows",
    "qt_environments",
    "qt_build",
    "qt_build_status",
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
    "qt_task",
}

if server.LAUNCH_SUPPORTED:
    # Launch-time injection exists only where the loader can preload a library.
    EXPECTED_TOOLS.add("qt_launch")


@pytest.mark.asyncio
async def test_mcp_instructions_cover_the_operating_loop():
    instructions = server.mcp.instructions or ""
    assert "When to use" in instructions
    assert "How to use" in instructions
    assert "Not for" in instructions
    assert "linked descriptor" in instructions
    assert "mode=\"injection\"" in instructions
    assert "qt_target_info" in instructions
    assert "qt_processes" in instructions
    assert "qt_query" in instructions
    assert "qt_attach" in instructions
    assert "qt_detach" in instructions
    assert "E_AMBIGUOUS" in instructions
    assert "E_HIDDEN" in instructions
    assert "include_hidden" in instructions
    assert "qt_methods" in instructions
    assert "qt_task" in instructions
    assert 'state": "running"' in instructions or "state" in instructions
    assert "wait_ms" in instructions
    assert "property setters" in " ".join(instructions.split())


@pytest.mark.asyncio
async def test_injection_recovery_contract_is_documented():
    """The failure paths an agent must recover from are stated up front."""
    instructions = " ".join((server.mcp.instructions or "").split())
    assert "restart_required" in instructions
    assert "loaded_injector" in instructions
    assert "for_pid" in instructions
    assert "error.reason" in instructions
    # The old text promised a build-id guard that does not exist.
    assert "rejects a build/architecture mismatch" not in instructions
    assert "no build-id validation" in instructions.lower()


@pytest.mark.asyncio
async def test_tool_descriptions_match_current_features():
    tools = {tool.name: tool for tool in await server.mcp.list_tools()}
    assert set(tools) == EXPECTED_TOOLS
    for tool in tools.values():
        assert tool.description and tool.description.strip()

    assert "set_geometry" in tools["qt_act"].description
    assert '"set":' in tools["qt_batch"].description
    assert '"get":' in tools["qt_batch"].description
    assert "batch_timeout" in tools["qt_batch"].description
    assert "Q_INVOKABLE" in tools["qt_call"].description
    assert "wait_ms" in tools["qt_act"].parameters["properties"]
    assert "wait_ms" in tools["qt_call"].parameters["properties"]
    assert "task_id" in tools["qt_task"].description
    assert "running" in tools["qt_task"].description
    assert "source_dir" in tools["qt_source"].description
    assert "E_NO_DEBUGINFO" in tools["qt_source"].description
    assert "pending" not in tools["qt_source"].description.lower()

    # Injection/build descriptions must carry the recovery contract.
    assert "restart_required" in tools["qt_attach"].description
    assert "injector_already_loaded" in tools["qt_attach"].description
    assert "guards against" not in tools["qt_attach"].description
    assert "for_pid" in tools["qt_build"].parameters["properties"]
    assert "for_pid" in tools["qt_build"].description
    assert "source_dir" in tools["qt_build"].parameters["properties"]
    assert "defaults to the qt-commander checkout" in (
        tools["qt_build"].description
    )
    assert "loaded_injector" in tools["qt_target_info"].description
    assert "restart_required" in tools["qt_target_info"].description
    assert "qt_commander" in tools["qt_environments"].description

    if server.LAUNCH_SUPPORTED:
        launch = tools["qt_launch"]
        assert "LD_PRELOAD" in launch.description
        assert "DYLD_INSERT_LIBRARIES" in launch.description
        assert "library" in launch.parameters["properties"]
        assert "program" in launch.parameters["required"]
        assert "qt_launch" in " ".join(
            (server.mcp.instructions or "").split()
        )
    else:
        # Windows has no preload hook: neither the tool nor the instructions
        # may advertise it.
        assert "qt_launch" not in tools
        assert "qt_launch" not in (server.mcp.instructions or "")

    items_params = tools["qt_items"].parameters
    assert "nth" in items_params["properties"]
    assert "object" in json.dumps(items_params["properties"]["owner"])
    assert "compact" in tools["qt_query"].parameters["properties"]
    assert "inherits" in tools["qt_query"].description
