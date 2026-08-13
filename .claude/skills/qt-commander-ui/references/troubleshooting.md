# Troubleshooting Decision Tree

Locate the problem by symptom, starting from the most likely cause.

## attach fails (2002)

Do the build parameters match the target process? → verify qt_major/toolchain/build_type/bitness with `qt_list_processes` (see build-params.md)
→ Do the artifacts exist? → `libqt-commander.dll` and `qt-injector.exe` under `.qt-commander/bin/`
→ MCP process running stale logic? → restart the MCP server (mandatory after source changes)

## snapshot times out

App busy → wait 5s and retry once
Still times out → the app may be stuck → check the app process, consider restarting it

## find returns nothing

- `include_hidden` not set? → hidden elements don't match by default; enable when needed
- objectName typo? → compare against the actual objectName in the snapshot
- Page not loaded? → wait for the page to finish loading before finding
- Custom QML component? → prefer `qml_id` (the QML `id` from source, shown on snapshot nodes); object_name/properties also work; type_inherits does NOT match QML type names

## click has no effect

- Is the element visible and unobstructed? → with occlusion, the click hits whatever covers it (cross-window occlusion is by design)
- Does the coordinate hit the element? → click_region uses the element center; when occluded/mis-aimed, retry with click_at for exact coordinates
- Still no effect → confirm the element is a real interactive component (a QML component needs its own MouseArea to handle clicks)

## keyboard input does not land

- Target not focused → click_region the target first to give it focus, then type
- Text landed in the wrong control → stale element id? re-find; QML apps have no focus widget, so the `element_id=0` fallback is unavailable

## stale element id errors

Only `qt_snapshot` renumbers ids; find/get_snapshot never do → if an id is stale, the element was destroyed or a snapshot ran since → re-find (or re-view) to recover the fresh id

## state looks wrong after a 2004 timeout

Snapshot first to assert the current state (the request may have executed) → apply corrective operations as needed; do not blindly retry

## target process crashed

Attach a debugger to reproduce: `cdb.exe -p <pid>` (symbol path pointing at the target app's Qt bin directory)
