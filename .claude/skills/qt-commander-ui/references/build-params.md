# qt_build Parameters vs Target Process Matching

When injection of qt_build artifacts fails (error 2002), the build parameters almost certainly don't match the target process. **Before every build, check the target process's qt_version/arch/bitness with `qt_list_processes` and verify each item.**

| Parameter | Must match | Notes |
|---|---|---|
| qt_major | Target process Qt major version | 5 or 6; cross-major injection always fails |
| toolchain | msvc / mingw | Same toolchain the target process was built with |
| build_type | Debug / Release | A Debug target process must use the Debug library and vice versa |
| bitness | 64 / 32 | Same architecture as the target process |

## Verification Steps

1. `qt_list_processes` → record the target process's `qt_version` / `arch` / `bitness`
2. Set each qt_build parameter to match, per the table above
3. Artifacts live in `<workspace>/.qt-commander/bin/`; the injected library's dependency closure must be complete (e.g. missing Qt5Quick/Qt5Qml/Qt5QuickWidgets fails attach)
4. On attach failure (2002), re-check this table; if parameters are correct, delete `.qt-commander/build_manifest.json` and retry (a long-running MCP process may compute the source hash with stale logic)
