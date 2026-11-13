"""The agent-facing surface of ``ops.act``: accepted values and what is sent."""

from __future__ import annotations

import pytest

from qt_commander import ops


class _RecordingSession:
    """Stands in for a Session; records what ops.act would put on the wire."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []

    async def call(self, method, params, *, timeout=None, op_id=None):
        self.calls.append((method, params))
        return {"ok": True, "dispatched": True}


@pytest.mark.asyncio
async def test_act_passes_button_modifiers_and_context_menu():
    session = _RecordingSession()
    await ops.act(
        session,
        "w1.2",
        "click",
        button="right",
        modifiers=["ctrl", "shift"],
        context_menu=True,
    )
    method, params = session.calls[0]
    assert method == "act"
    assert params["button"] == "right"
    assert params["modifiers"] == ["ctrl", "shift"]
    assert params["context_menu"] is True


@pytest.mark.asyncio
async def test_act_accepts_every_documented_button():
    for button in sorted(ops.MOUSE_BUTTONS):
        session = _RecordingSession()
        await ops.act(session, "w1.2", "click", button=button)
        assert session.calls[0][1]["button"] == button


@pytest.mark.asyncio
async def test_act_rejects_an_unknown_button():
    session = _RecordingSession()
    with pytest.raises(ValueError, match="button must be one of"):
        await ops.act(session, "w1.2", "click", button="sideways")
    assert session.calls == []


@pytest.mark.asyncio
async def test_act_rejects_an_unknown_modifier():
    session = _RecordingSession()
    with pytest.raises(ValueError, match="modifier must be one of"):
        await ops.act(session, "w1.2", "click", modifiers=["hyper"])
    assert session.calls == []


@pytest.mark.asyncio
async def test_act_accepts_the_new_actions():
    for action in ("press", "release", "context_menu"):
        assert action in ops.ACTIONS
        session = _RecordingSession()
        await ops.act(session, "w1.2", action)
        assert session.calls[0][1]["action"] == action


def test_defaults_are_unobtrusive():
    """Right-click is not a context menu: the flag defaults to off."""
    import inspect

    signature = inspect.signature(ops.act)
    assert signature.parameters["button"].default == "left"
    assert signature.parameters["context_menu"].default is False
