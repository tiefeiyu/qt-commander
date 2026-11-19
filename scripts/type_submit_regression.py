"""Destructive-input regression: a synthetic event must never outlive its receiver.

Guards the Qt Creator crash of 0.10.0: `qt_act(type, submit=true)` on the
Options/Devices host-name field took the IDE down. Enter accepted the modal
dialog, the page that owned the QLineEdit was destroyed, and qt-commander then
dispatched the matching KeyRelease to the freed widget. Qt's own
`QCoreApplication::notifyInternal2` dereferences `receiver->d_func()` before any
event handler runs, so the target died with an access violation inside
Qt5Cored.dll. The same hole existed for the KeyPress/KeyRelease pair of a single
typed character, for an editable child of the addressed container, and for the
multi-event `hover` dispatch; this suite covers all of them.

The target is `tests/test-apps/modal`, whose hostile modes tear the editor down
from inside the keystroke (or hover event) that was delivered to it, at the last
point where the fixture itself is not doing undefined behaviour: right after the
widget handled the event, still inside `QApplication::notify`. The normal modes
of that app stay intact for `task_regression.py`. Build it once:

    cmake -S tests/test-apps/modal -B build/modal-msvc5 -G Ninja \\
          -DCMAKE_BUILD_TYPE=Debug -DQT_MAJOR_VERSION=5 \\
          -DCMAKE_PREFIX_PATH=C:/Software/Qt/5.15.2/msvc2019_64

A debug Qt runtime makes the *pre-fix* failure deterministic (the debug CRT fills
freed blocks, so the stale event faults); the fixed suite passes on a release
runtime as well.

Example:

    .venv\\Scripts\\python.exe scripts/type_submit_regression.py
    .venv\\Scripts\\python.exe scripts/type_submit_regression.py \\
        --exe build/modal-msvc5/qt-modal-test.exe \\
        --qt-bin C:\\Software\\Qt\\5.15.2\\msvc2019_64\\bin

Exit code 0 means every scenario survived and reported the documented result.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from task_regression import node_id, run_mode

from qt_commander import ops
from qt_commander.client import ClientClosed, ClientTimeout, RpcError
from qt_commander.session import Session

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / "build" / "modal-msvc5" / "qt-modal-test.exe"

TYPED = "10.12.2.201"


class TargetGone(AssertionError):
    """The target process died while the scenario was running."""


class Probe:
    """Wraps the RPC calls so a target death is always attributed to a step."""

    def __init__(self, session: Session, process: subprocess.Popen, label: str) -> None:
        self.session = session
        self.process = process
        self.label = label

    def alive(self, where: str) -> None:
        code = self.process.poll()
        if code is not None:
            raise TargetGone(f"[{self.label}] target died {where} (exit code {code})")

    def _failure(self, what: str, exc: Exception) -> AssertionError:
        """Distinguish a crash from a hang: both deserve different follow-ups."""
        code = self.process.poll()
        detail = f"{type(exc).__name__}: {exc}"
        if isinstance(exc, ClientClosed):
            # The connection dropped: the target process is gone (or exiting),
            # even when poll() has not reaped it yet.
            return TargetGone(
                f"[{self.label}] target died {what} (exit {code}): {detail}"
            )
        if code is None:
            return AssertionError(
                f"[{self.label}] target still alive but {what} did not answer: {detail}"
            )
        return TargetGone(f"[{self.label}] target died {what} (exit {code}): {detail}")

    async def act(self, target: str | Mapping[str, Any], action: str, **kwargs) -> dict:
        try:
            result = await ops.act(self.session, target, action, **kwargs)
        except (ClientClosed, ClientTimeout) as exc:
            raise self._failure(f"during {action} {target!r}", exc) from exc
        self.alive(f"during {action} {target!r}")
        return result

    async def call(self, method: str, params: dict, timeout: float = 10.0) -> dict:
        try:
            result = await self.session.call(method, params, timeout=timeout)
        except (ClientClosed, ClientTimeout) as exc:
            raise self._failure(f"during {method}", exc) from exc
        self.alive(f"during {method}")
        return result

    async def ping(self) -> None:
        result = await self.call("ping", {}, timeout=2.0)
        assert result.get("pong") is True, result

    async def label_text(self, object_name: str) -> str:
        result = await self.call(
            "get", {"target": {"object_name": object_name}, "props": ["text"]}
        )
        return (result.get("props") or {}).get("text", "")

    async def query_count(self, object_name: str) -> int:
        result = await self.call(
            "query",
            {
                "where": {"object_name": object_name},
                "include_hidden": True,
                "count_only": True,
            },
        )
        return int(result.get("count") or 0)

    async def stale(self, node: str, action: str, **kwargs) -> None:
        """The destroyed widget's id must be rejected, not dereferenced."""
        await self.expect_error("E_STALE_ID", action, {"id": node}, **kwargs)

    async def expect_error(
        self, code: str, action: str, target: str | Mapping[str, Any], **kwargs
    ) -> None:
        try:
            result = await ops.act(self.session, target, action, **kwargs)
        except RpcError as exc:
            assert exc.code == code, (
                f"[{self.label}] expected {code}, got {exc.code}: {exc.message}"
            )
            self.alive(f"while rejecting {action} with {code}")
            return
        error = result.get("error")
        if isinstance(error, dict):
            assert error.get("code") == code, result
            return
        if isinstance(error, str):
            assert error == code, result
            return
        raise AssertionError(
            f"[{self.label}] expected {code} for {target!r}, got {result}"
        )


async def open_editor(probe: Probe) -> str:
    """Click the button that opens the modal editor and return the edit's id."""
    open_id = await node_id(probe.session, "btnOpenEditor")
    pending = await probe.act({"id": open_id}, "click", wait_ms=200)
    assert pending.get("state") == "running", pending
    assert pending.get("task_id"), pending
    return await node_id(probe.session, "lineEditHost")


async def scenario_submit_destroys_editor(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    edit_id = await open_editor(probe)

    # Control (report TC3): the same text without submit keeps the target alive.
    typed = await probe.act(
        {"id": edit_id}, "type", clear=True, text=TYPED, submit=False
    )
    assert typed.get("ok") is True, typed
    assert typed.get("text_after") == TYPED, typed
    assert (typed.get("resolved") or {}).get("gone") is not True, typed
    assert await probe.label_text("editorStatus") == f"text:{TYPED}"

    # The crash (report TC1): Enter accepts the dialog, the page that owns the
    # QLineEdit is destroyed inside the KeyPress, the KeyRelease must not go out.
    killed = await probe.act(
        {"id": edit_id},
        "type",
        clear=True,
        text=TYPED,
        submit=True,
        expect={"gone": True},
    )
    assert killed.get("ok") is True, killed
    assert killed.get("text_after") == TYPED, killed
    resolved = killed.get("resolved") or {}
    assert resolved.get("gone") is True, killed
    assert resolved.get("id") == edit_id, killed
    # An agent follows a destructive action with `expect gone`; that poll must
    # reach the dead receiver's id and match, not error.
    assert (killed.get("expect") or {}).get("matched") is True, killed

    await probe.ping()
    assert await probe.query_count("lineEditHost") == 0
    await probe.stale(edit_id, "type", text="x")

    # The same stale id inside a batch is an error in the step, not a crash.
    batched = await ops.batch(
        session,
        [
            {"act": {"target": {"id": edit_id}, "action": "type", "text": "x"}},
            {
                "wait": {
                    "condition": {
                        "where": {"object_name": "lineEditHost"},
                        "gone": True,
                    },
                    "timeout_ms": 200,
                }
            },
        ],
    )
    assert batched.get("ok") is False, batched
    step_error = batched["steps"][0].get("error") or {}
    assert step_error.get("code") == "E_STALE_ID", batched
    print(f"{label} submit-destroys-editor: survived, gone={edit_id}")


async def scenario_destroys_child(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    assert await probe.query_count("containerHost") == 1
    edit_id = await node_id(session, "lineEditHost")

    # The addressed target is the container; the editable child is what dies.
    typed = await probe.act(
        {"object_name": "containerHost"}, "type", clear=True, text=TYPED, submit=False
    )
    assert typed.get("ok") is True, typed
    assert typed.get("text_after") == TYPED, typed

    killed = await probe.act(
        {"object_name": "containerHost"}, "type", clear=True, text=TYPED, submit=True
    )
    assert killed.get("ok") is True, killed
    assert killed.get("text_after") == TYPED, killed
    resolved = killed.get("resolved") or {}
    assert resolved.get("gone") is True, killed
    assert resolved.get("id") == edit_id, killed

    await probe.ping()
    # The container outlived its child and stays addressable.
    assert await probe.query_count("containerHost") == 1
    assert await probe.query_count("lineEditHost") == 0
    await probe.stale(edit_id, "type", text="x")
    print(f"{label} destroys-child: survived, container intact")


async def scenario_destroys_container(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    """Enter takes the addressed container and the editor down together."""
    probe = Probe(session, process, label)
    container_id = await node_id(session, "containerHost")
    edit_id = await node_id(session, "lineEditHost")

    killed = await probe.act(
        {"id": container_id}, "type", clear=True, text=TYPED, submit=True
    )
    assert killed.get("ok") is True, killed
    assert killed.get("text_after") == TYPED, killed
    resolved = killed.get("resolved") or {}
    assert resolved.get("gone") is True, killed
    assert resolved.get("id") == edit_id, killed
    # The addressed target died too, and the observation names *it*.
    observed = killed.get("observed") or {}
    assert observed.get("gone") is True, killed
    assert observed.get("id") == container_id, killed

    await probe.ping()
    assert await probe.query_count("containerHost") == 0
    assert await probe.query_count("lineEditHost") == 0
    await probe.stale(container_id, "type", text="x")
    await probe.stale(edit_id, "type", text="x")
    print(f"{label} destroys-container: survived, both anchors gone")


async def scenario_release_destroys_child(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    """The KeyRelease kills the child; the addressed container stays alive.

    The character was already inserted by the KeyPress, so `text_after` must
    report it, and the next character must not be dispatched.
    """
    probe = Probe(session, process, label)
    edit_id = await node_id(session, "lineEditHost")

    typed = await probe.act(
        {"object_name": "containerHost"},
        "type",
        clear=True,
        text="aXb",
        submit=False,
    )
    assert typed.get("ok") is True, typed
    resolved = typed.get("resolved") or {}
    assert resolved.get("gone") is True, typed
    assert resolved.get("id") == edit_id, typed
    assert typed.get("text_after") == "aX", typed

    await probe.ping()
    assert await probe.query_count("containerHost") == 1
    assert await probe.query_count("lineEditHost") == 0
    await probe.stale(edit_id, "type", text="x")
    print(f"{label} release-destroys-child: survived, text_after=aX")


async def scenario_texteditors(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    """QTextEdit and QPlainTextEdit go through the same receiver guard."""
    probe = Probe(session, process, label)
    for object_name, class_name in (
        ("plainEditorHost", "QPlainTextEdit"),
        ("richEditorHost", "QTextEdit"),
    ):
        typed = await probe.act(
            {"object_name": object_name}, "type", clear=True, text="abc"
        )
        assert typed.get("ok") is True, typed
        assert typed.get("text_after") == "abc", typed
        assert (typed.get("resolved") or {}).get("class") == class_name, typed
        assert (typed.get("resolved") or {}).get("gone") is not True, typed
    await probe.ping()
    print(f"{label} texteditors: QTextEdit and QPlainTextEdit typed")


async def scenario_rebuilds_editor(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    edit_id = await open_editor(probe)

    replaced = await probe.act(
        {"id": edit_id}, "type", clear=True, text=TYPED, submit=True
    )
    assert replaced.get("ok") is True, replaced
    assert (replaced.get("resolved") or {}).get("gone") is True, replaced
    await probe.ping()

    # The page was rebuilt in place: one fresh editor with a fresh id.
    assert await probe.query_count("lineEditHost") == 1
    assert await probe.query_count("editorPage") == 0
    assert await probe.query_count("editorPageRebuilt") == 1
    new_id = await node_id(session, "lineEditHost")
    assert new_id != edit_id, (new_id, edit_id)
    await probe.stale(edit_id, "type", text="x")

    typed = await probe.act(
        {"id": new_id}, "type", clear=True, text="10.0.0.1", submit=False
    )
    assert typed.get("ok") is True, typed
    assert typed.get("text_after") == "10.0.0.1", typed

    # The rebuild hook fires again: a second destructive submit works, and
    # observe=false must drop the observation without losing `resolved.gone`.
    kill_again = await probe.act(
        {"id": new_id},
        "type",
        clear=True,
        text="10.0.0.2",
        submit=True,
        observe=False,
    )
    assert kill_again.get("ok") is True, kill_again
    assert (kill_again.get("resolved") or {}).get("gone") is True, kill_again
    assert "observed" not in kill_again, kill_again
    await probe.ping()
    assert await probe.query_count("lineEditHost") == 1
    assert await node_id(session, "lineEditHost") not in (edit_id, new_id)
    print(f"{label} rebuilds-editor: {edit_id} -> {new_id} -> rebuilt again")


async def scenario_closes_plain(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    edit_id = await open_editor(probe)

    # Enter accepts the dialog but nothing is destroyed: no false "gone".
    closed = await probe.act(
        {"id": edit_id}, "type", clear=True, text=TYPED, submit=True
    )
    assert closed.get("ok") is True, closed
    assert closed.get("text_after") == TYPED, closed
    assert (closed.get("resolved") or {}).get("gone") is not True, closed
    await probe.ping()
    assert await probe.query_count("lineEditHost") == 1
    assert await node_id(session, "lineEditHost") == edit_id
    print(f"{label} closes-plain: survived, editor kept")


async def scenario_enter_keeps_open(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    edit_id = await node_id(session, "lineEditHost")

    submitted = await probe.act(
        {"id": edit_id}, "type", clear=True, text=TYPED, submit=True
    )
    assert submitted.get("ok") is True, submitted
    assert submitted.get("text_after") == TYPED, submitted
    assert (submitted.get("resolved") or {}).get("gone") is not True, submitted
    # The Return really arrived: the fixture's handler saw it.
    assert await probe.label_text("submittedLabel") == f"submitted:{TYPED}"

    plain = await probe.act(
        {"id": edit_id}, "type", clear=True, text="abc", submit=False
    )
    assert plain.get("ok") is True, plain
    assert plain.get("text_after") == "abc", plain
    assert await probe.label_text("submittedLabel") == f"submitted:{TYPED}"

    await probe.expect_error(
        "E_NOT_EDITABLE", "type", {"object_name": "notEditableLabel"}, text="x"
    )
    print(f"{label} enter-keeps-open: submit and plain typing both fine")


async def scenario_char_destroys_editor(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    probe = Probe(session, process, label)
    edit_id = await node_id(session, "lineEditHost")

    # 'X' tears the editor down while it is being typed, on the KeyPress. The
    # release for 'X' and every later character must not be delivered: a run
    # that completes proves they were not (a later event would hit the freed
    # widget and kill the target on a debug runtime). `text_after` is the last
    # text observed while the widget was alive ("a"; 'X' died with it).
    typed = await probe.act(
        {"id": edit_id}, "type", clear=True, text="aXb", submit=False
    )
    assert typed.get("ok") is True, typed
    assert (typed.get("resolved") or {}).get("gone") is True, typed
    assert typed.get("resolved", {}).get("id") == edit_id, typed
    assert typed.get("text_after") == "a", typed
    await probe.ping()
    # The fixture recorded the editor's contents at teardown time: 'X' was
    # inserted before it died, so the character path ran to completion.
    assert await probe.label_text("editorGoneLabel") == "editor:gone aX"
    assert await probe.query_count("lineEditHost") == 0
    await probe.stale(edit_id, "type", text="x")
    print(f"{label} char-destroys-editor: survived mid-typing teardown")


async def scenario_hover_destroys_target(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    """The Enter event deletes the widget that received it.

    `hover` sends Enter then HoverEnter (and later HoverMove/Leave) to the same
    widget; none of the later events may be dispatched once the first one killed
    the receiver.
    """
    probe = Probe(session, process, label)
    host_id = await node_id(session, "hoverHost")
    await probe.expect_error("E_STALE_ID", "hover", {"id": host_id})
    await probe.ping()
    assert await probe.query_count("hoverHost") == 0
    print(f"{label} hover-destroys-target: survived mid-hover teardown")


async def scenario_key_return(
    session: Session, process: subprocess.Popen, label: str
) -> None:
    """The `key` action already re-checks between press and release."""
    probe = Probe(session, process, label)
    edit_id = await open_editor(probe)

    pressed = await probe.act({"id": edit_id}, "key", keys=["Return"])
    assert pressed.get("ok") is True, pressed
    assert (pressed.get("observed") or {}).get("gone") is True, pressed
    await probe.ping()
    assert await probe.query_count("lineEditHost") == 0
    print(f"{label} key-return: survived, observed gone")


SCENARIOS = (
    # Pinning scenarios: each one crashes or misreports against the pre-fix
    # runtime. The controls (closes-plain, enter-keeps-open, key-return) pass
    # either way and guard against an over-eager "gone"/stale result.
    ("type-submit-destroys-editor", scenario_submit_destroys_editor),
    ("type-submit-destroys-editor", scenario_key_return),  # control
    ("type-submit-destroys-child", scenario_destroys_child),
    ("type-submit-destroys-container", scenario_destroys_container),
    ("type-char-release-destroys-child", scenario_release_destroys_child),
    ("type-char-destroys-editor", scenario_char_destroys_editor),
    ("type-submit-rebuilds-editor", scenario_rebuilds_editor),
    ("type-texteditors", scenario_texteditors),
    ("hover-destroys-target", scenario_hover_destroys_target),
    ("type-submit-closes-plain", scenario_closes_plain),  # control
    ("type-enter-keeps-open", scenario_enter_keeps_open),  # control
)


async def main(exe: Path, label: str, only: str | None) -> int:
    if not exe.is_file():
        raise SystemExit(f"target not built: {exe}")
    selected = [
        (mode, scenario)
        for mode, scenario in SCENARIOS
        if not only or only in mode or only in scenario.__name__
    ]
    if not selected:
        print(f"no scenario matches {only!r}", file=sys.stderr)
        return 2
    failures: list[str] = []
    for mode, scenario in selected:
        name = f"{mode}/{scenario.__name__}"
        try:
            await run_mode(exe, mode, scenario, label)
        except (TargetGone, AssertionError) as exc:
            failures.append(f"[{name}] {exc}")
            print(f"FAIL [{name}] {exc}", file=sys.stderr)
        except Exception as exc:  # noqa: BLE001 - report, never mask a mode
            failures.append(f"[{name}] {type(exc).__name__}: {exc}")
            print(f"FAIL [{name}] {type(exc).__name__}: {exc}", file=sys.stderr)
    if failures:
        print(f"type/submit regression: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print(f"type/submit regression: OK ({len(selected)} scenarios)")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--label", default="input")
    parser.add_argument(
        "--only", default=None, help="run modes/scenarios matching this substring"
    )
    parser.add_argument(
        "--qt-bin", default=None, help="directory holding the target's Qt runtime DLLs"
    )
    arguments = parser.parse_args()
    if arguments.qt_bin:
        os.environ["PATH"] = arguments.qt_bin + os.pathsep + os.environ.get("PATH", "")
    raise SystemExit(
        asyncio.run(main(arguments.exe.resolve(), arguments.label, arguments.only))
    )
