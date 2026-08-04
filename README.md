# qt-commander

MCP server for Qt application introspection and automation — the
[Playwright](https://playwright.dev) for native Qt, covering both
QWidget and QML interfaces.

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

## Quick Start

```bash
# Install (pulls fastmcp and other runtime dependencies automatically)
pip install -e .

# Launch MCP server
python -m qt_commander
```

> **AI Agent / MCP 客户端配置**：详见 [llms-install.md](llms-install.md)。

## Building

The injector and library require **Visual Studio 2022+**, **Qt 5.15+**, and
**CMake 3.16+**. They can be compiled manually or on-demand via the `qt_build`
MCP tool.

### Manual build

```powershell
# Setup environment
cmd /c "C:\...\vcvars64.bat" amd64
cmd /c "C:\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat"

# Configure & build
cmake -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_LIBRARY=ON ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/msvc
```

### On-demand build (via MCP)

```python
# 1. Discover MSVC and Qt installations on this machine
qt_detect_msvc_and_qt()

# 2. Pick paths from the result and build
qt_build(
    qt_env="C:/Qt/5.15.2/msvc2019_64/bin/qtenv2.bat",
    vcvars_path="C:/.../vcvars64.bat",
    vcvars_args="amd64"
)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_INJECTOR` | ON | Build `qt-injector.exe` |
| `BUILD_LIBRARY` | OFF | Build `libqt-commander.dll` (auto-enabled with `BUILD_TESTS`) |
| `BUILD_TESTS` | ON | Build all test suites |
| `WITH_QML` | ON | Enable QML/QQuick support |
| `QT_MAJOR_VERSION` | 5 | Qt major version (5 or 6) |

## MCP Tools

| Tool | Description |
|------|-------------|
| `qt_list_processes` | List running Qt processes (cross-platform via psutil) |
| `qt_attach` | Inject library into a target process and open a session |
| `qt_detach` | Disconnect from a session, optionally eject the library |
| `qt_list_sessions` | List active sessions |
| `qt_detect_msvc_and_qt` | Auto-detect MSVC and Qt installations available for building |
| `qt_build` | Compile injector + library on demand |
| `qt_snapshot` | Capture the full UI element tree |
| `qt_find_element` | Find elements by type, text, or property query |
| `qt_get_property` | Read a QObject property |
| `qt_set_property` | Write a QObject property |
| `qt_call_method` | Invoke a QObject method |
| `qt_screenshot` | Capture a screenshot of a specific element or window |
| `qt_mouse_click` | Send a mouse click to a UI element (direct delivery) |
| `qt_mouse_click_at` | Click at an exact window coordinate — routed through the real Qt input pipeline (QPA), with real scene-graph/widget hit testing, identical to a human click |
| `qt_mouse_click_region` | Click at the center of an element's on-screen region — real hit testing decides the actual target (e.g. a QML Rectangle's MouseArea) |
| `qt_keyboard_input` | Send keyboard input |
| `qt_focus` | Set focus on a specific element |

## Testing

Everything (C++ unit + E2E suites, pytest, and the deployment-level preload
verification) runs from one CMake build tree:

```powershell
# Single build tree (injector + library + test apps + all tests)
cmake -S . -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_TESTS=ON -DWITH_QML=ON ^
  -DCMAKE_BUILD_TYPE=Release -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5

# Build everything, then run ALL tests in one command:
cmake --build build/msvc
ctest --test-dir build/msvc --output-on-failure
```

`ctest` runs 20 suites: 14 injector C++ suites (including real E2E
injection against the widget test app), 4 library C++ suites, the full
pytest suite (`python_unit_tests`), and the E2E preload verification
(`verify_preload`, labeled `e2e`, which needs the `qt_build` artifacts in
`.qt-commander/bin`).

Quick subsets:

```bash
pytest tests/ -q                                   # Python only
ctest --test-dir build/msvc -LE e2e                # skip slow E2E
ctest --test-dir build/msvc -R "test_selector"     # one suite
```

### Test matrix (verified state, 2026-08)

| Suite | Location | Language | Tests | Requires |
|-------|----------|----------|-------|----------|
| Server unit | `tests/unit_server/` | Python | 245 | Python 3.10+ |
| Injector unit | `tests/unit_injector/` | C++ | 276 | MSVC |
| Library unit | `tests/unit_library/` | C++ | 55 | MSVC + Qt |
| E2E injection | `tests/unit_injector/test_e2e*.cpp` | C++ | 16 | MSVC + Qt + test app |
| E2E preload | `tests/verify_preload.py` | Python | 3 scenarios | qt_build artifacts |

## Project Structure

```
qt-commander/
├── qt_commander/              Python MCP server
│   ├── server.py            FastMCP app, 16 tools + 2 resources
│   ├── session.py           Session/SessionManager with RPC lock
│   ├── rpc_client.py        Subprocess injector launcher
│   ├── builder.py           On-demand MSVC build orchestrator
│   ├── process_detector.py  Cross-platform Qt process discovery
│   ├── environment_detector.py  MSVC/Qt build environment auto-detection
│   ├── framing.py           4-byte BE length-prefix frame protocol
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
│       ├── core/            UI scanner, event injector, screenshot, element map
│       ├── protocol/        JSON-RPC handler
│       ├── rpc/             TCP RPC server
│       └── selector/        Element query engine
│
├── tests/
│   ├── unit_server/         Python unit tests (245)
│   ├── unit_injector/       C++ unit + E2E tests (276)
│   ├── unit_library/        C++ library component tests (55)
│   ├── verify_preload.py    E2E: dependency preload scenarios A/B/C
│   └── test-apps/           Minimal Qt test applications
│
└── CMakeLists.txt
```

## License

MIT
