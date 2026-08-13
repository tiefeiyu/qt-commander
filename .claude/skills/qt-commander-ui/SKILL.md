---
name: qt-commander-ui
description: Drive a running Qt application's UI to complete verification loops (attach/snapshot/find/click/keyboard input/screenshot). Use when a task needs to interact with a real Qt app UI (click buttons, type text, verify UI state, capture screenshot evidence); excludes pure backend/log verification (use the project's own verify-style flows) and process management (use the project's own run/kill-style flows).
allowed-tools: mcp__qt-commander__*
---

# qt-commander-ui

Drive a running Qt application's UI to complete verification loops. All operations go through the qt-commander MCP tools (configured at user level, available in every project).

## When to Use

- Verification tasks that need to click buttons, type text, verify UI state, or capture screenshot evidence
- Not for: pure backend/log verification or process start/stop management (use the project's own verify/run/kill-style skills)

## Preflight

1. **Build the library** (first time or after source changes):
   - `qt_detect_msvc_and_qt` to discover the environment → confirm build parameters with the user (**never auto-pick**)
   - `qt_build` (parameters must match the target process; see references/build-params.md)
2. **Start the target app and wait for its main window before attaching** (during startup the GUI is busy; injection and operations tend to time out with 2004)
3. `qt_attach <pid>` (find the pid with `qt_list_processes`) → confirm `connected` via `qt_list_sessions`

## Standard Workflow

1. `qt_snapshot(max_depth=1, detail=extended)` → read the snapshot resource (`qt-commander://sessions/...`) to inspect the UI tree
2. `qt_find_element` to locate the target (custom QML components: prefer `qml_id` — the QML `id` from source, shown on snapshot nodes — then object_name/properties; type_inherits does NOT match QML type names)
3. **Ids are stable until the next `qt_snapshot`** — `qt_find_element` and `qt_get_snapshot` never invalidate them; hold onto ids across steps
4. **Need full-tree context after a find?** Call `qt_get_snapshot` (read-only view) — it never renumbers ids, so the ids you already hold stay valid. Only an explicit `qt_snapshot` renumbers.
5. Capture a `qt_screenshot` after operations as evidence

## Operating Discipline (mandatory)

1. **Mouse clicks always go through the real input pipeline**: `qt_mouse_click_region` (element center) or `qt_mouse_click_at` (exact coordinates) — routed through Qt's real event dispatch + hit testing, identical to a human click (QML MouseArea, focus changes, etc. all work). **No QWidget/QML distinction.**
2. `qt_mouse_click` (direct delivery to the widget) is a debug fallback only; not for routine use.
3. `qt_keyboard_input` is already real input; always use a fresh id — a stale id errors out (`Element not found: id=N`) instead of silently typing.
4. **2004 timeout ≠ failure**: the request may still execute (at-least-once). **Do not blindly retry** side-effect operations (click/type/setProperty); snapshot first to verify state.
5. Prefer real input; avoid `qt_set_property`/`qt_call_method` (destructive, bypasses the real interaction path).
6. Before rebuilding the library, `qt_detach(purge=True)` (the DLL is locked by the target process, failing the build).
7. 2007 = definitively not executed, safe to retry (unlike 2004; see references/error-codes.md).

## Verification Loop Template

snapshot baseline → perform operation → snapshot/find to assert state change → screenshot evidence → report

## References

- `references/error-codes.md` — error code reference (incl. 2007 vs 2004 retry semantics)
- `references/build-params.md` — build parameters vs target process matching
- `references/troubleshooting.md` — troubleshooting decision tree
