# qt-commander

MCP server for Qt application introspection and automation — the
[Playwright](https://playwright.dev) for native Qt, covering both
QWidget and QML interfaces.

![Qt 5.15](https://img.shields.io/badge/Qt-5.15-blue)
![Qt 6.8](https://img.shields.io/badge/Qt-6.8-brightgreen)
![MSVC](https://img.shields.io/badge/MSVC-supported-blue)
![MinGW](https://img.shields.io/badge/MinGW-supported-blue)
![Platform](https://img.shields.io/badge/Windows-supported-important)
![License](https://img.shields.io/badge/License-Apache%202.0-orange)

## Why qt-commander

AI agents (Claude, Cursor, …) can drive **native Qt applications** the
way Playwright drives web pages:

- **No source changes** — the library is injected into a *running*
  process; you control any Qt app (yours or a third party's) as-is.
- **Both UI stacks** — QWidget *and* QML/Qt Quick, Qt 5.15 and Qt 6.8,
  MSVC and MinGW.
- **What the agent gets** — full UI snapshots with geometry, z-order,
  visibility, opacity and properties; occlusion-pruned views of what a
  human actually sees; element lookup by text / type / property;
  real input pipeline clicks, typing, keyboard shortcuts, drags.
- **Easy to try** — `uv run python -m qt_commander`; the injector and
  library compile on demand against any detected Qt kit.

## Quick Start

```bash
# Launch MCP server via uv (no global Python install needed — uv resolves
# pyproject.toml / uv.lock and manages an isolated environment)
uv run python -m qt_commander
```

Requires [uv](https://docs.astral.sh/uv) and Python 3.10+.

### MCP client configuration

**Claude Code** — add to `.mcp.json` / user-scope MCP config:

```json
{
  "mcpServers": {
    "qt-commander": {
      "command": "uv",
      "args": ["run", "python", "-m", "qt_commander"],
      "cwd": "path/to/qt-commander"
    }
  }
}
```

**Cursor / other MCP clients** — same command shape; the server speaks
stdio MCP and needs no other setup.  See [llms-install.md](llms-install.md)
for the full install guide (including removing legacy pip installs).

## MCP Tools

| Tool | Description |
|------|-------------|
| `qt_list_processes` | List running Qt processes (cross-platform via psutil) |
| `qt_attach` | Inject library into a target process and open a session |
| `qt_detach` | Disconnect from a session, optionally eject the library |
| `qt_list_sessions` | List active sessions |
| `qt_detect_msvc_and_qt` | Auto-detect MSVC, MinGW toolchains, and Qt installations available for building |
| `qt_build` | Compile injector + library on demand — `toolchain` selects `msvc` (vcvars + qtenv bat) or `mingw` (MinGW bin dir + Qt bin dir) |
| `qt_snapshot` | Capture the UI element tree — `detail` selects the property tier: `core` (geometry/visibility/text, no properties), `extended` (common interaction state), `full` (every Q_PROPERTY) |
| `qt_prune_snapshot` | Occlusion-prune a snapshot: remove elements fully covered by higher-z opaque elements (equal z ordered by creation, later covers earlier; children paint above their parent), mark partially covered ones with `visible_ratio`, write a compact pruned snapshot |
| `qt_find_element` | Find elements by type, text, or property query |
| `qt_get_property` | Read a QObject property |
| `qt_set_property` | Write a QObject property |
| `qt_call_method` | Invoke a QObject method |
| `qt_screenshot` | Capture a screenshot of a specific element or window |
| `qt_mouse_click` | Send a mouse click to a UI element (direct delivery) |
| `qt_mouse_click_at` | Click at an exact window coordinate — routed through the real Qt input pipeline (QPA), with real scene-graph/widget hit testing, identical to a human click |
| `qt_mouse_click_region` | Click at the center of an element's on-screen region — real hit testing decides the actual target (e.g. a QML Rectangle's MouseArea) |
| `qt_mouse_press` | Press a mouse button on an element without releasing it |
| `qt_mouse_release` | Release a previously pressed mouse button (completes a click or a drag) |
| `qt_mouse_move` | Move the pointer to an element-local position (drag = press → move → release) |
| `qt_keyboard_input` | Send keyboard input (typed text, optionally with held modifiers) |
| `qt_key_combo` | Send a shortcut such as `Ctrl+C` or `Ctrl+Shift+A` (real press/release pair with modifiers) |
| `qt_focus` | Set focus on a specific element |

## Architecture

```
┌──────────┐     stdio      ┌──────────────┐   subprocess    ┌──────────────┐
│ AI Agent │ ◄────────────► │ MCP Server   │ ──────────────► │ qt-injector  │
└──────────┘                │ (Python)     │                 │ (C++)        │
                            └──────────────┘                 └──────┬───────┘
                                                                    │
                                                    CreateRemoteThread
                                                                    │
                                                           ┌────────▼───────┐
                                                           │ libqt-commander│
                                                           │ (C++/Qt)       │
                                                           └────────────────┘
```

| Component | Path | Language | Role |
|-----------|------|----------|------|
| MCP Server | `qt_commander/` | Python | Protocol bridge, session management, on-demand build |
| Injector CLI | `src/injector/` | C++ | Standalone binary that loads the library into a target process |
| Injection Library | `src/library/` | C++/Qt | In-process engine for UI introspection, manipulation, capture |
| Shared | `src/common/` | C++ | Frame protocol, TCP socket utilities |

### How it works

1. **AI Agent** sends an MCP tool call (e.g. `qt_snapshot`) via stdio.
2. **MCP Server** spawns `qt-injector.exe` as a subprocess with the target PID.
3. **qt-injector** loads `libqt-commander.dll` into the target Qt process via
   `CreateRemoteThread` + `LoadLibraryW`.  Before injecting the library it
   **preloads the library's transitive dependency closure** (Qt DLLs the
   target app does not link, e.g. Qt5Widgets for a pure QML app) from the
   library's own directory — no manual Qt DLL copies next to the target
   executable are needed.  It then performs a token-authenticated handshake
   and prints the library's TCP port to stdout.
4. **MCP Server** connects to the library over TCP and relays RPC calls
   (snapshot, click, input, etc.) using a 4-byte length-prefix frame protocol.

## Testing

Everything (C++ unit + E2E suites, pytest, and the deployment-level preload
verification) runs from one CMake build tree:

```powershell
# Single build tree (injector + library + test apps + all tests).
# Both Qt5 and Qt6 are supported; pick the Qt you want to validate:
#   Qt5 MSVC: -DQT_MAJOR_VERSION=5 -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5
#   Qt6 MSVC: -DQT_MAJOR_VERSION=6 -DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6
#   Qt6 MinGW: same, but Qt6_DIR=C:/Qt/6.8.3/mingw_64/lib/cmake/Qt6
#              with the MinGW toolchain on PATH (see "MinGW" below)
cmake -S . -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_TESTS=ON -DWITH_QML=ON ^
  -DCMAKE_BUILD_TYPE=Release -DQT_MAJOR_VERSION=6 ^
  -DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6

# Build everything, then run ALL tests in one command:
cmake --build build/msvc
ctest --test-dir build/msvc --output-on-failure
```

`verify_preload` (E2E deployment checks) auto-detects the Qt major AND
toolchain kit (msvc/mingw) of the deployed `libqt-commander.dll` and
verifies the matching DLL set — even windeployqt follows the deployment's
kit — so the same script validates Qt5/Qt6 × msvc/mingw deployments from
either build tree.

## MinGW

MinGW builds are fully supported (Qt5 and Qt6 MinGW kits). Use
`qt_build` with `toolchain="mingw"`: pass the MinGW toolchain's bin dir
as `vcvars_path` and the kit's `qtenv2.bat` as `qt_env` (MinGW Qt kits
ship qtenv2.bat just like MSVC kits). `qt_detect_msvc_and_qt` reports
MinGW toolchains (`mingw_toolchains`) and tags each Qt kit with its
`kit` ("msvc"/"mingw").

Notes:

- **Compiler version**: Qt 5's official MinGW kit ships GCC 8.1, whose
  libstdc++ cannot compile `std::filesystem` headers (fixed in 8.3); use
  a GCC ≥ 9 toolchain (e.g. Qt's bundled `mingw1310_64`) for Qt 5 too.
  `qt_build` pins the compiler explicitly (`-DCMAKE_C/CXX_COMPILER`), so
  other gcc builds on PATH (e.g. a Strawberry Perl toolchain) never get
  picked up.
- **Runtime DLLs**: MinGW executables need `libgcc_s_seh-1.dll`,
  `libstdc++-6.dll`, `libwinpthread-1.dll` next to them. The build
  deploys the compiler's own runtime (a Qt kit's older runtime lacks
  newer symbols), and `verify_preload` matches the deployed app's runtime
  to the library's.
- **Kit matching**: the injector library, its deployment, and the target
  application must share the same Qt kit (all MSVC or all MinGW) —
  mixing kits loads two Qt module sets into one process and breaks the
  preload closure.

`ctest` runs 19 suites: 14 injector C++ suites (including three real E2E
injection suites against the widget test app), 3 library C++ suites, the
full pytest suite (`python_unit_tests`), and the E2E preload verification
(`verify_preload`, labeled `e2e`, which needs the `qt_build` artifacts in
`.qt-commander/bin`).

The E2E suites auto-anchor their working directory to their own build
tree (`test_util.h::chdir_to_exe_dir`), so they produce identical results
when launched directly from the repo root or through `ctest`.

Quick subsets:

```bash
pytest tests/ -q                                   # Python only
ctest --test-dir build/msvc -LE e2e                # skip slow E2E
ctest --test-dir build/msvc -R "test_selector"     # one suite
```

### Test matrix (verified state, 2026-08)

| Suite | Location | Language | Tests | Requires |
|-------|----------|----------|-------|----------|
| Server unit | `tests/unit_server/` | Python | 254 | Python 3.10+ |
| Injector unit | `tests/unit_injector/` | C++ | 326 | MSVC |
| Library unit | `tests/unit_library/` | C++ | 43 | MSVC + Qt |
| E2E injection | `tests/unit_injector/test_e2e*.cpp` | C++ | 48 | MSVC + Qt + test app |
| E2E preload | `tests/verify_preload.py` | Python | 3 scenarios | qt_build artifacts |

## Project Structure

```
qt-commander/
├── qt_commander/              Python MCP server
│   ├── server.py            FastMCP app, 22 tools + 2 resources
│   ├── session.py           Session/SessionManager with RPC lock
│   ├── rpc_client.py        Subprocess injector launcher
│   ├── builder.py           On-demand MSVC build orchestrator
│   ├── process_detector.py  Cross-platform Qt process discovery
│   ├── environment_detector.py  MSVC/Qt build environment auto-detection
│   ├── framing.py           4-byte BE length-prefix frame protocol
│   ├── occlusion.py         Snapshot occlusion solving (drop covered
│   │                        elements, mark visible ratio)
│   └── errors.py            MCP error code registry
│
├── src/
│   ├── common/              Shared C++ utilities
│   │   ├── framing.h        Frame protocol (header-only)
│   │   ├── socket_utils.h   TCP abstraction
│   │   └── socket_utils.cpp
│   ├── injector/            Standalone injection CLI
│   │   ├── main.cpp         Entry point, argument parsing, --list-deps, exit codes 1-6
│   │   ├── injector.h       Public API declarations
│   │   ├── injector_win.cpp Win32 implementation (CreateRemoteThread, PE
│   │   │                    import parser, dependency-closure preload)
│   │   ├── injector_di.cpp  DI variants (IProcessOps-driven, fully testable)
│   │   └── os_ops.h         IProcessOps / MockProcessOps / Win32ProcessOps
│   └── library/             Injected DLL
│       ├── entry_win.cpp    DllMain / Windows entry
│       ├── api.h            InitParams handshake layout (1024 bytes)
│       ├── compat_qt.h      Qt5/Qt6 compatibility macros
│       ├── core/            UI scanner, event injector, screenshot, element map
│       ├── rpc/             TCP RPC server (JSON-RPC handler)
│       └── selector/        Element query engine
│
├── tests/
│   ├── unit_server/         Python unit tests (254)
│   ├── unit_injector/       C++ unit + E2E tests (326)
│   ├── unit_library/        C++ library component tests (43)
│   ├── verify_preload.py    E2E: dependency preload scenarios A/B/C
│   └── test-apps/           Minimal Qt test applications
│
└── CMakeLists.txt
```

## License

MIT
