"""Unit tests for session registry and typed request builders."""
from __future__ import annotations

import time

import pytest

from qt_commander import ops
from qt_commander.client import ClientTimeout, FrameResponse, RpcError, ServerHello
from qt_commander.session import (
    Session,
    SessionExists,
    SessionManager,
    SessionNotFound,
)

HELLO = ServerHello(
    protocol=2,
    library_version="2.0.0",
    capabilities=("qml", "expect_js"),
    session_tag="7f3a",
    max_inflight=8,
    max_frame=67108864,
)


class FakeClient:
    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []
        self.frames: list[tuple[str, dict]] = []
        self.timeouts: list[float | None] = []
        self.query_counts: list[int] = []
        self.act_results: list[dict] = []
        self.fail_methods: dict[str, Exception] = {}
        self.closed = False

    async def call(self, method, params=None, *, timeout=None, op_id=None):
        self.calls.append((method, dict(params or {})))
        self.timeouts.append(timeout)
        if method in self.fail_methods:
            raise self.fail_methods[method]
        if method == "query":
            count = self.query_counts.pop(0) if self.query_counts else 1
            return {"count": count}
        if method == "act":
            return self.act_results.pop(0) if self.act_results else {"ok": True}
        return {"ok": True, "method": method}

    async def call_frame(self, method, params=None, *, timeout=None, op_id=None):
        self.frames.append((method, dict(params or {})))
        self.timeouts.append(timeout)
        return FrameResponse(result={"format": "png"}, binary=b"PNG")

    async def close(self) -> None:
        self.closed = True


def make_session() -> tuple[Session, FakeClient]:
    client = FakeClient()
    return Session("s1", 4321, client, HELLO), client  # type: ignore[arg-type]


@pytest.mark.asyncio
async def test_query_builds_documented_params():
    session, client = make_session()
    await ops.query(
        session,
        where={"class": "QPushButton"},
        props=["text"],
        coords="screen",
        max_depth=2,
        limit=5,
        include_hidden=True,
        occlusion=True,
    )
    assert client.calls == [
        (
            "query",
            {
                "where": {"class": "QPushButton"},
                "props": ["text"],
                "coords": "screen",
                "max_depth": 2,
                "limit": 5,
                "include_hidden": True,
                "occlusion": True,
                "count_only": False,
            },
        )
    ]


@pytest.mark.asyncio
async def test_query_rejects_unknown_coords():
    session, _ = make_session()
    with pytest.raises(ValueError, match="coords"):
        await ops.query(session, coords="desktop")


@pytest.mark.asyncio
async def test_act_builds_documented_params_and_omits_unset():
    session, client = make_session()
    await ops.act(
        session,
        "7f3a.42",
        "click",
        position={"x": 1, "y": 2},
        expect={"where": {"object_name": "x"}},
    )
    method, params = client.calls[0]
    assert method == "act"
    assert params["target"] == "7f3a.42"
    assert params["action"] == "click"
    assert params["position"] == {"x": 1, "y": 2}
    assert "expect" not in params
    assert params["nth"] == 0
    assert params["input"] == "auto"
    assert params["observe"] is True
    assert "text" not in params
    result = await ops.act(
        session, "7f3a.42", "click",
        expect={"where": {"object_name": "x"}},
    )
    assert result["expect"]["matched"] is True


@pytest.mark.asyncio
async def test_act_expect_times_out_without_dispatch_failure():
    session, client = make_session()
    client.query_counts = [0] * 20
    result = await ops.act(
        session,
        "7f3a.42",
        "click",
        expect={"where": {"object_name": "never"}},
        timeout_ms=50,
    )
    assert result["ok"] is False
    assert result["cause"] == "expect_not_met"
    assert result["dispatched"] is True


@pytest.mark.asyncio
async def test_act_expect_rpc_error_keeps_dispatched():
    session, client = make_session()
    client.fail_methods["query"] = RpcError("E_NOT_FOUND", "gone")
    result = await ops.act(
        session,
        "7f3a.42",
        "click",
        expect={"where": {"object_name": "gone"}},
        timeout_ms=50,
    )
    assert result["ok"] is False
    assert result["cause"] == "expect_error"
    assert result["dispatched"] is True
    assert result["expect"]["error"]["code"] == "E_NOT_FOUND"


@pytest.mark.asyncio
async def test_act_rejects_unknown_action():
    session, _ = make_session()
    with pytest.raises(ValueError, match="action"):
        await ops.act(session, "7f3a.42", "select")


@pytest.mark.asyncio
async def test_act_rpc_deadline_covers_expect_and_settle_budget():
    session, client = make_session()
    await ops.act(session, "7f3a.42", "click", timeout_ms=60_000, settle_ms=500)
    # expect polling owns timeout_ms; the RPC only carries settle/duration.
    assert client.timeouts[-1] >= 5.5


@pytest.mark.asyncio
async def test_items_and_property_ops():
    session, client = make_session()
    await ops.items(session, "7f3a.42", limit=10, offset=20)
    await ops.items(session, "7f3a.42", compact=True)
    await ops.set_property(session, "7f3a.42", "text", "hello")
    await ops.call_method(session, "7f3a.42", "click", [1])
    assert client.calls == [
        (
            "items",
            {
                "owner": "7f3a.42",
                "limit": 10,
                "offset": 20,
                "include_hidden": False,
            },
        ),
        (
            "items",
            {
                "owner": "7f3a.42",
                "limit": 50,
                "offset": 0,
                "include_hidden": False,
                "compact": True,
            },
        ),
        ("set", {"target": "7f3a.42", "name": "text", "value": "hello"}),
        ("call", {"target": "7f3a.42", "method": "click", "args": [1]}),
    ]


@pytest.mark.asyncio
async def test_screenshot_returns_frame_with_binary():
    session, client = make_session()
    response = await ops.screenshot(
        session, max_width=0, annotate="matched", where={"class": "QDialog"}
    )
    assert response.binary == b"PNG"
    method, params = client.frames[0]
    assert method == "screenshot"
    assert params["max_width"] == 0
    assert params["annotate"] == "matched"
    assert params["format"] == "png"
    assert "target" not in params


@pytest.mark.asyncio
async def test_screenshot_rejects_unknown_annotate():
    session, _ = make_session()
    with pytest.raises(ValueError, match="annotate"):
        await ops.screenshot(session, annotate="boxes")


@pytest.mark.asyncio
async def test_source_params():
    session, client = make_session()
    await ops.source(session, "7f3a.42", handler="onClicked", snippet_lines=5)
    assert client.calls == [
        (
            "source",
            {
                "target": "7f3a.42",
                "handler": "onClicked",
                "snippet_lines": 5,
                "max_lines": 20,
            },
        )
    ]


@pytest.mark.asyncio
async def test_wait_polls_until_match():
    session, client = make_session()
    client.query_counts = [0, 0, 1]
    result = await ops.wait(
        session,
        {"where": {"class": "QDialog"}},
        timeout_ms=500,
        poll_ms=10,
    )
    assert result["matched"] is True
    assert result["count"] == 1
    assert len(client.calls) == 3
    assert all(call[0] == "query" for call in client.calls)
    assert all(call[1]["count_only"] is True for call in client.calls)


@pytest.mark.asyncio
async def test_wait_timeout_returns_unmatched():
    session, client = make_session()
    client.query_counts = [0]
    result = await ops.wait(
        session, {"where": {"id": "x"}}, timeout_ms=0, poll_ms=10
    )
    assert result["matched"] is False
    assert result["count"] == 0
    assert len(client.calls) == 1


@pytest.mark.asyncio
async def test_wait_gone_checks_absence():
    session, client = make_session()
    client.query_counts = [1, 0]
    result = await ops.wait(
        session,
        {"where": {"id": "x"}, "gone": True},
        timeout_ms=500,
        poll_ms=10,
    )
    assert result["matched"] is True
    assert result["count"] == 0


@pytest.mark.asyncio
async def test_wait_rejects_js_and_gone_without_where():
    session, _ = make_session()
    with pytest.raises(ValueError, match="js"):
        await ops.wait(session, {"js": "$ui.count() === 0"})
    with pytest.raises(ValueError, match="where"):
        await ops.wait(session, {"gone": True})


@pytest.mark.asyncio
async def test_batch_executes_act_then_wait():
    session, client = make_session()
    client.act_results = [{"ok": True}]
    client.query_counts = [1]
    result = await ops.batch(
        session,
        [
            {"act": {"target": "x", "action": "click"}},
            {"wait": {"condition": {"where": {"id": "x"}}}},
        ],
    )
    assert result["ok"] is True
    assert len(result["steps"]) == 2
    assert [call[0] for call in client.calls] == ["act", "query"]


@pytest.mark.asyncio
async def test_batch_stops_on_failure():
    session, client = make_session()
    client.act_results = [{"ok": False}]
    result = await ops.batch(
        session,
        [
            {"act": {"target": "x", "action": "click"}},
            {"act": {"target": "y", "action": "click"}},
        ],
    )
    assert result["ok"] is False
    assert len(result["steps"]) == 1
    assert len(client.calls) == 1


@pytest.mark.asyncio
async def test_batch_rejects_invalid_step():
    session, _ = make_session()
    with pytest.raises(ValueError, match="unknown step kind"):
        await ops.batch(session, [{"noop": {}}])


@pytest.mark.asyncio
async def test_batch_validates_all_steps_before_dispatching():
    session, client = make_session()
    with pytest.raises(ValueError, match="unknown step kind"):
        await ops.batch(
            session,
            [
                {"act": {"target": "x", "action": "click"}},
                {"noop": {}},
            ],
        )
    assert client.calls == []


@pytest.mark.asyncio
async def test_batch_preserves_completed_steps_on_transport_error():
    session, client = make_session()
    client.fail_methods["act"] = ClientTimeout(1, "act", 0.05)
    result = await ops.batch(
        session,
        [{"act": {"target": "x", "action": "click"}}],
    )
    assert result["ok"] is False
    assert len(result["steps"]) == 1
    assert result["steps"][0]["error"]["code"] == "E_TARGET_TIMEOUT"


@pytest.mark.asyncio
async def test_wait_returns_unmatched_on_query_timeout():
    session, client = make_session()
    client.fail_methods["query"] = ClientTimeout(1, "query", 0.05)
    result = await ops.wait(
        session, {"where": {"object_name": "x"}}, timeout_ms=50, poll_ms=10
    )
    assert result["matched"] is False
    assert result["count"] == 0


@pytest.mark.asyncio
async def test_session_connected_follows_client_state():
    session, client = make_session()
    assert session.connected is True
    await session.close()
    assert session.connected is False
    assert client.closed is True


@pytest.mark.asyncio
async def test_session_grace_window():
    session, _ = make_session()
    assert session.grace_remaining_ms == 0
    await session.close()
    assert 0 < session.grace_remaining_ms <= 60_000
    session.disconnected_at = time.monotonic() - 61
    assert session.grace_remaining_ms == 0
    manager = SessionManager()
    manager.add(session)
    with pytest.raises(SessionNotFound):
        manager.get("s1")


def test_session_manager_add_get_list_remove():
    manager = SessionManager()
    session, _ = make_session()
    manager.add(session)
    assert manager.get("s1") is session
    info = manager.list()
    assert len(info) == 1
    assert info[0].session_id == "s1"
    assert info[0].pid == 4321
    assert info[0].capabilities == ("qml", "expect_js")
    with pytest.raises(SessionExists):
        manager.add(session)
    assert manager.remove("s1") is session
    with pytest.raises(SessionNotFound):
        manager.get("s1")


@pytest.mark.asyncio
async def test_session_manager_close_all():
    manager = SessionManager()
    session, client = make_session()
    manager.add(session)
    await manager.close_all()
    assert client.closed is True
    assert manager.list() == []
