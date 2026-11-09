"""Typed request builders for the RPC methods.

Tools are thin: validate the public enum values, build exactly the documented
params, and delegate.  Keeping this in one module prevents query/act/get from
drifting into subtly different payloads.
"""
from __future__ import annotations

import asyncio
import time
from typing import Any, Mapping

from .client import ClientClosed, ClientTimeout, FrameResponse, RpcError
from .errors import ErrorCode
from .session import Session

ACTIONS = frozenset(
    {
        "click",
        "dblclick",
        "hover",
        "type",
        "key",
        "focus",
        "restore",
        "minimize",
        "set_geometry",
        "scroll",
        "drag",
    }
)
COORDS = frozenset({"parent", "screen"})
ANNOTATE = frozenset({"none", "interactive", "matched", "all", "layers"})
SCREENSHOT_MAX_BYTES = 6 * 1024 * 1024
IMAGE_FORMATS = frozenset({"png"})
INPUT_MODES = frozenset({"auto", "pointer", "api"})
# The RPC deadline must outlive the action's own expect/settle budget;
# otherwise a long expect is reported as an RPC timeout after dispatch.
ACT_RPC_MARGIN_S = 5.0


def _choice(name: str, value: str, allowed: frozenset[str]) -> str:
    if value not in allowed:
        raise ValueError(
            f"{name} must be one of {sorted(allowed)}, got {value!r}"
        )
    return value


def _clean(params: Mapping[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in params.items() if value is not None}


def _expect_conditions(
    expect: Mapping[str, Any] | list[Mapping[str, Any]],
) -> list[Mapping[str, Any]]:
    conditions = expect if isinstance(expect, list) else [expect]
    if not conditions:
        raise ValueError("expect must not be empty")
    for condition in conditions:
        if not isinstance(condition, Mapping):
            raise ValueError("expect conditions must be objects")
        unknown = set(condition) - {"where", "props", "gone", "js"}
        if unknown:
            raise ValueError(f"unsupported expect keys: {sorted(unknown)}")
        if "js" in condition:
            raise ValueError("JS expect is not implemented")
        if "gone" in condition and "props" in condition:
            raise ValueError("expect gone and props are mutually exclusive")
        if not any(key in condition for key in ("where", "props", "gone")):
            raise ValueError("expect needs where, props or gone")
    return conditions


async def _expect_once(
    session: Session,
    condition: Mapping[str, Any],
    resolved_id: str | None,
    timeout: float,
) -> tuple[bool, dict[str, Any] | None]:
    where = condition.get("where")
    if not isinstance(where, Mapping):
        where = {"id": resolved_id} if resolved_id else {}
    if condition.get("gone") is True:
        result = await session.call(
            "query",
            {"where": dict(where), "count_only": True},
            timeout=timeout,
        )
        return result.get("count") == 0, None
    if "props" in condition:
        expected = condition["props"]
        if not isinstance(expected, Mapping):
            raise ValueError("expect props must be an object")
        result = await session.call(
            "get",
            {"target": dict(where), "props": list(expected)},
            timeout=timeout,
        )
        actual = result.get("props", {})
        for key, value in expected.items():
            if actual.get(key) != value:
                return False, result
        return True, result
    result = await session.call(
        "query",
        {"where": dict(where), "count_only": True},
        timeout=timeout,
    )
    return int(result.get("count", 0)) > 0, None


async def _poll_expect(
    session: Session,
    conditions: list[Mapping[str, Any]],
    resolved_id: str | None,
    timeout_ms: int,
) -> tuple[bool, int, dict[str, Any] | None, dict[str, Any] | None]:
    deadline = time.monotonic() + timeout_ms / 1000.0
    started = time.monotonic()
    last_node = None
    last_error = None
    while True:
        matched = True
        for condition in conditions:
            remaining = max(0.05, deadline - time.monotonic())
            try:
                ok, node = await _expect_once(
                    session, condition, resolved_id, remaining
                )
            except ClientTimeout:
                ok, node = False, None
            except RpcError as exc:
                return False, int((time.monotonic() - started) * 1000), \
                    last_node, {
                        "code": exc.code,
                        "message": exc.message,
                    }
            if node is not None:
                last_node = node
            if not ok:
                matched = False
                break
        after_ms = int((time.monotonic() - started) * 1000)
        if matched:
            return True, after_ms, last_node, None
        if time.monotonic() >= deadline:
            return False, after_ms, last_node, last_error
        await asyncio.sleep(0.05)


async def windows(
    session: Session,
    *,
    include_hidden: bool = False,
    visible: bool | None = None,
    class_name: str | None = None,
    limit: int = 200,
    offset: int = 0,
) -> dict[str, Any]:
    params: dict[str, Any] = {
        "include_hidden": include_hidden,
        "limit": limit,
        "offset": offset,
    }
    if visible is not None:
        params["visible"] = visible
    if class_name is not None:
        params["class"] = class_name
    return await session.call("windows", params)


async def query(
    session: Session,
    *,
    where: Mapping[str, Any] | None = None,
    scope: Mapping[str, Any] | None = None,
    props: Any = None,
    coords: str = "parent",
    max_depth: int = -1,
    limit: int = 200,
    include_hidden: bool = False,
    occlusion: bool = False,
    count_only: bool = False,
    compact: bool = False,
) -> dict[str, Any]:
    _choice("coords", coords, COORDS)
    if max_depth < -1:
        raise ValueError("max_depth must be >= -1")
    if limit < 1:
        raise ValueError("limit must be >= 1")
    params = _clean(
        {
            "where": where,
            "scope": scope,
            "props": props,
            "coords": coords,
            "max_depth": max_depth,
            "limit": limit,
            "include_hidden": include_hidden,
            "occlusion": occlusion,
            "count_only": count_only,
        }
    )
    if compact:
        params["compact"] = True
    result = await session.call("query", params)
    # A filtered miss is ambiguous to the caller: the node may exist but be
    # pruned as hidden. Probe once (count_only, cheap) and surface the hint.
    if (
        where
        and not include_hidden
        and not count_only
        and isinstance(result.get("count"), int)
        and result["count"] == 0
    ):
        probe = await session.call(
            "query",
            {**params, "include_hidden": True, "count_only": True},
        )
        hidden = probe.get("count") or 0
        if isinstance(hidden, int) and hidden > 0:
            result["hidden_matched"] = hidden
            result["hint"] = (
                f"{hidden} node(s) match but are hidden; "
                "pass include_hidden=true to include them"
            )
    return result


async def items(
    session: Session,
    owner: str | Mapping[str, Any],
    *,
    where: Mapping[str, Any] | None = None,
    limit: int = 50,
    offset: int = 0,
    include_hidden: bool = False,
    compact: bool = False,
    nth: int | None = None,
) -> dict[str, Any]:
    if limit < 1:
        raise ValueError("limit must be >= 1")
    if offset < 0:
        raise ValueError("offset must be >= 0")
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    payload = {
        "owner": owner,
        "where": where,
        "limit": limit,
        "offset": offset,
        "include_hidden": include_hidden,
        "nth": nth,
    }
    if compact:
        payload["compact"] = True
    return await session.call("items", _clean(payload))


async def act(
    session: Session,
    target: str | Mapping[str, Any],
    action: str,
    *,
    nth: int | None = None,
    position: Mapping[str, float] | None = None,
    geometry: Mapping[str, float] | None = None,
    button: str = "left",
    modifiers: list[str] | None = None,
    text: str | None = None,
    clear: bool = False,
    submit: bool = False,
    keys: list[str] | None = None,
    dx: float = 0.0,
    dy: float = 0.0,
    to: str | Mapping[str, Any] | None = None,
    duration_ms: int = 300,
    expect: Mapping[str, Any] | list[Mapping[str, Any]] | None = None,
    timeout_ms: int = 2000,
    settle_ms: int = 0,
    observe: bool = True,
    input: str = "auto",
    include_hidden: bool = False,
    js_risk_ack: bool = False,
) -> dict[str, Any]:
    _choice("action", action, ACTIONS)
    _choice("input", input, INPUT_MODES)
    expect_conditions = _expect_conditions(expect) if expect is not None else None
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    if timeout_ms < 0:
        raise ValueError("timeout_ms must be >= 0")
    if settle_ms < 0:
        raise ValueError("settle_ms must be >= 0")
    if duration_ms < 0:
        raise ValueError("duration_ms must be >= 0")
    params = _clean(
        {
            "target": target,
            "action": action,
            "nth": nth,
            "position": position,
            "geometry": geometry,
            "button": button,
            "modifiers": modifiers,
            "text": text,
            "clear": clear,
            "submit": submit,
            "keys": keys,
            "dx": dx,
            "dy": dy,
            "to": to,
            "duration_ms": duration_ms,
            "timeout_ms": timeout_ms,
            "settle_ms": settle_ms,
            "observe": observe,
            "input": input,
            "js_risk_ack": js_risk_ack,
        }
    )
    if include_hidden:
        params["include_hidden"] = True
    rpc_timeout = max(
        1.0,
        (settle_ms + duration_ms) / 1000.0 + ACT_RPC_MARGIN_S,
    )
    result = await session.call("act", params, timeout=rpc_timeout)
    if expect_conditions is None or result.get("ok") is False:
        return result
    resolved = result.get("resolved")
    resolved_id = (
        resolved.get("id") if isinstance(resolved, Mapping) else
        (target if isinstance(target, str) else None)
    )
    matched, after_ms, node, error = await _poll_expect(
        session, expect_conditions, resolved_id, timeout_ms
    )
    expect_result: dict[str, Any] = {
        "matched": matched,
        "after_ms": after_ms,
    }
    if node is not None:
        expect_result["node"] = node
    if error is not None:
        expect_result["error"] = error
    result["expect"] = expect_result
    if not matched:
        result["ok"] = False
        result["cause"] = "expect_error" if error else "expect_not_met"
        result["retryable"] = False
        result["dispatched"] = True
    return result


async def get(
    session: Session,
    target: str | Mapping[str, Any],
    *,
    props: Any = None,
    nth: int | None = None,
    include_hidden: bool = False,
) -> dict[str, Any]:
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    params = _clean({"target": target, "props": props, "nth": nth})
    if include_hidden:
        params["include_hidden"] = True
    return await session.call("get", params)


async def set_property(
    session: Session,
    target: str | Mapping[str, Any],
    name: str,
    value: Any,
    nth: int | None = None,
    include_hidden: bool = False,
) -> dict[str, Any]:
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    params = _clean({"target": target, "name": name, "value": value,
                     "nth": nth})
    if include_hidden:
        params["include_hidden"] = True
    return await session.call("set", params)


async def call_method(
    session: Session,
    target: str | Mapping[str, Any],
    method: str,
    args: list[Any] | None = None,
    nth: int | None = None,
    signature: str | None = None,
    include_hidden: bool = False,
) -> dict[str, Any]:
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    params = _clean({"target": target, "method": method, "args": args,
                     "nth": nth, "signature": signature})
    if include_hidden:
        params["include_hidden"] = True
    return await session.call("call", params)


async def screenshot(
    session: Session,
    *,
    target: str | Mapping[str, Any] | None = None,
    nth: int | None = None,
    max_width: int = 1024,
    annotate: str = "none",
    where: Mapping[str, Any] | None = None,
    max_labels: int = 50,
    annotate_offset: int = 0,
    format: str = "png",
) -> FrameResponse:
    _choice("annotate", annotate, ANNOTATE)
    _choice("format", format, IMAGE_FORMATS)
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    if max_width < 0:
        raise ValueError("max_width must be >= 0 (0 = native)")
    if max_labels < 0:
        raise ValueError("max_labels must be >= 0")
    if annotate_offset < 0:
        raise ValueError("annotate_offset must be >= 0")
    params = _clean(
        {
            "target": target,
            "nth": nth,
            "max_width": max_width,
            "annotate": annotate,
            "where": where,
            "max_labels": max_labels,
            "annotate_offset": annotate_offset,
            "format": format,
        }
    )
    frame = await session.call_frame("screenshot", params)
    if max_width > 0 and len(frame.binary) > SCREENSHOT_MAX_BYTES:
        requested = max_width
        width = max_width
        while width > 256:
            width = max(256, int(width * 0.75))
            params["max_width"] = width
            frame = await session.call_frame("screenshot", params)
            if len(frame.binary) <= SCREENSHOT_MAX_BYTES:
                break
        frame.result["requested_max_width"] = requested
        frame.result["auto_downscaled"] = True
    return frame


async def source(
    session: Session,
    target: str | Mapping[str, Any],
    *,
    handler: str | None = None,
    snippet_lines: int = 3,
    max_lines: int = 20,
    nth: int | None = None,
    include_hidden: bool = False,
    source_dir: str | None = None,
) -> dict[str, Any]:
    if snippet_lines < 0 or max_lines < 1:
        raise ValueError("snippet_lines must be >= 0 and max_lines >= 1")
    if nth is not None and nth < 0:
        raise ValueError("nth must be >= 0")
    params = _clean(
        {
            "target": target,
            "nth": nth,
            "handler": handler,
            "snippet_lines": snippet_lines,
            "max_lines": max_lines,
            "source_dir": source_dir,
        }
    )
    if include_hidden:
        params["include_hidden"] = True
    return await session.call("source", params)


def _wait_where(condition: Mapping[str, Any]) -> tuple[dict[str, Any], bool]:
    if not isinstance(condition, Mapping):
        raise TypeError("condition must be an object")
    unknown = set(condition) - {"where", "props", "gone", "js"}
    if unknown:
        raise ValueError(f"unknown condition keys: {sorted(unknown)}")
    if "js" in condition:
        raise ValueError("js conditions are not supported by this client yet")
    gone = condition.get("gone", False)
    if not isinstance(gone, bool):
        raise ValueError("gone must be a boolean")
    where = condition.get("where") or {}
    if not isinstance(where, Mapping):
        raise ValueError("where must be an object")
    where = dict(where)
    props = condition.get("props")
    if props is not None:
        if gone:
            raise ValueError("gone cannot be combined with props")
        if not isinstance(props, Mapping):
            raise ValueError("props must be an object")
        existing = where.get("props") or {}
        if not isinstance(existing, Mapping):
            raise ValueError("where.props must be an object")
        where["props"] = {**existing, **dict(props)}
    if not where:
        raise ValueError("condition needs where or props")
    if gone and "props" in where:
        raise ValueError("gone cannot be combined with props")
    return where, gone


async def wait(
    session: Session,
    condition: Mapping[str, Any],
    *,
    timeout_ms: int = 2000,
    poll_ms: int = 75,
) -> dict[str, Any]:
    """Poll a declarative condition until it matches or the deadline expires.

    JS conditions are rejected here; they belong to the action engine once it
    can evaluate them, not to the Python-side poller.
    """
    if timeout_ms < 0:
        raise ValueError("timeout_ms must be >= 0")
    if not 10 <= poll_ms <= 500:
        raise ValueError("poll_ms must be between 10 and 500")
    where, gone = _wait_where(condition)

    start = time.monotonic()
    deadline = start + timeout_ms / 1000.0
    while True:
        remaining = max(0.0, deadline - time.monotonic())
        try:
            result = await session.call(
                "query",
                {"where": where, "count_only": True},
                timeout=max(0.05, remaining),
            )
        except ClientTimeout:
            return {
                "matched": False,
                "count": 0,
                "waited_ms": int((time.monotonic() - start) * 1000),
            }
        count = result.get("count")
        if type(count) is not int:
            raise ValueError("query count_only must return an integer count")
        matched = count == 0 if gone else count > 0
        elapsed_ms = int((time.monotonic() - start) * 1000)
        if matched:
            return {"matched": True, "count": count, "waited_ms": elapsed_ms}
        if elapsed_ms >= timeout_ms:
            return {"matched": False, "count": count, "waited_ms": elapsed_ms}
        remaining_s = max(0.0, deadline - time.monotonic())
        await asyncio.sleep(min(poll_ms / 1000.0, remaining_s))


def _batch_error(exc: BaseException) -> dict[str, Any]:
    if isinstance(exc, RpcError):
        error: dict[str, Any] = {
            "code": exc.code,
            "message": exc.message,
            "retryable": exc.retryable,
        }
        if exc.hint:
            error["hint"] = exc.hint
        if exc.details:
            error["details"] = exc.details
        return {"ok": False, "error": error}
    if isinstance(exc, ClientTimeout):
        return {
            "ok": False,
            "error": {
                "code": ErrorCode.TARGET_TIMEOUT.value,
                "message": str(exc),
                "retryable": False,
            },
        }
    if isinstance(exc, ClientClosed):
        return {
            "ok": False,
            "error": {
                "code": ErrorCode.LINKED_UNREACHABLE.value,
                "message": str(exc),
                "retryable": False,
            },
        }
    code = (
        ErrorCode.INVALID_ARGUMENT.value
        if isinstance(exc, (ValueError, TypeError))
        else ErrorCode.INVALID_EXPECT.value
    )
    return {"ok": False, "error": {"code": code, "message": str(exc)}}


def _validate_step(step: Mapping[str, Any]) -> tuple[str, Mapping[str, Any]]:
    if not isinstance(step, Mapping) or len(step) != 1:
        raise ValueError(
            "each step must have exactly one of: act, wait, set, get"
        )
    kind, payload = next(iter(step.items()))
    if kind not in {"act", "wait", "set", "get"}:
        raise ValueError(f"unknown step kind: {kind!r}")
    if not isinstance(payload, Mapping):
        raise ValueError(f"{kind} payload must be an object")
    if kind in {"act", "set", "get"} and "target" not in payload:
        raise ValueError(f"{kind} target is required")
    if kind == "act":
        _choice("action", str(payload.get("action", "")), ACTIONS)
        _choice("input", str(payload.get("input", "auto")), INPUT_MODES)
    elif kind == "set":
        if "name" not in payload or "value" not in payload:
            raise ValueError("set name and value are required")
    elif kind == "wait":
        condition = payload.get("condition")
        if not isinstance(condition, Mapping):
            raise ValueError("wait condition is required")
        _wait_where(condition)
    return kind, payload


async def batch(
    session: Session,
    steps: list[Mapping[str, Any]],
    *,
    stop_on_error: bool = True,
    timeout_ms: int = 30000,
    max_steps: int = 50,
) -> dict[str, Any]:
    """Execute act/wait/set/get steps sequentially within one budget.

    The whole batch shares a single deadline so a long sequence cannot outlive
    the MCP caller's timeout: every step runs with the remaining time, wait
    steps are clamped to it, and an exhausted budget returns the completed
    steps plus ``cause="batch_timeout"`` instead of losing everything.
    """
    if not isinstance(steps, list):
        raise TypeError("steps must be a list")
    if not 100 <= timeout_ms <= 60000:
        raise ValueError("timeout_ms must be between 100 and 60000")
    if not 1 <= max_steps <= 500:
        raise ValueError("max_steps must be between 1 and 500")
    if len(steps) > max_steps:
        raise ValueError(
            f"batch has {len(steps)} steps; max_steps is {max_steps}"
        )
    validated = [_validate_step(step) for step in steps]
    planned_wait = sum(
        int(payload.get("timeout_ms", 2000))
        for kind, payload in validated
        if kind == "wait"
    )
    if planned_wait > timeout_ms:
        raise ValueError(
            f"wait steps alone may block {planned_wait} ms, above the batch "
            f"budget of {timeout_ms} ms; raise timeout_ms or shorten the waits"
        )
    started = time.monotonic()
    deadline = started + timeout_ms / 1000.0
    results: list[dict[str, Any]] = []
    ok = True
    cause: str | None = None
    for kind, payload in validated:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            ok = False
            cause = "batch_timeout"
            break
        step_payload = dict(payload)
        if kind == "wait":
            step_payload["timeout_ms"] = min(
                int(step_payload.get("timeout_ms", 2000)),
                max(1, int(remaining * 1000)),
            )
        try:
            if kind == "act":
                step = act(session, **step_payload)
            elif kind == "set":
                step = set_property(session, **step_payload)
            elif kind == "get":
                step = get(session, **step_payload)
            else:
                step = wait(session, **step_payload)
            result = await asyncio.wait_for(step, timeout=remaining)
        except asyncio.TimeoutError:
            result = {
                "ok": False,
                "error": {
                    "code": ErrorCode.BATCH_TIMEOUT.value,
                    "message": f"batch exceeded its {timeout_ms} ms budget "
                               f"during {kind}",
                    "retryable": False,
                },
            }
            results.append(result)
            ok = False
            cause = "batch_timeout"
            break
        except (RpcError, ClientTimeout, ClientClosed, ValueError, TypeError) as exc:
            result = _batch_error(exc)
        results.append(result)
        failed = result.get("ok") is False or result.get("matched") is False
        if failed:
            ok = False
            if stop_on_error:
                break
    payload_result: dict[str, Any] = {"ok": ok, "steps": results}
    if cause:
        payload_result["cause"] = cause
        payload_result["timeout_ms"] = timeout_ms
        payload_result["elapsed_ms"] = int((time.monotonic() - started) * 1000)
    return payload_result

