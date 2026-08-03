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
   `CreateRemoteThread` + `LoadLibraryW`, performs a token-authenticated
   handshake, then prints the library's TCP port to stdout.
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
| `qt_mouse_click` | Send a mouse click event |
| `qt_keyboard_input` | Send keyboard input |
| `qt_focus` | Set focus on a specific element |

## Testing

```bash
# Python only — no compiler needed
python scripts/run_all_tests.py

# Full suite — Python + C++ (requires MSVC + Qt)
python scripts/run_all_tests.py ^
  --vcvars "C:\...\vcvars64.bat" ^
  --qt-env "C:\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat"

# Quick mode — skip CMake configure
python scripts/run_all_tests.py --quick --vcvars "..." --qt-env "..."
```

### Test matrix

| Suite | Location | Language | Tests | Requires |
|-------|----------|----------|-------|----------|
| Server unit | `tests/unit_server/` | Python | 169 | Python 3.10+ |
| Injector unit | `tests/unit_injector/` | C++ | ~100 | MSVC |
| Library unit | `tests/unit_library/` | C++ | ~36 | MSVC + Qt |
| E2E integration | `tests/unit_injector/test_e2e*.cpp` | C++ | ~30 | MSVC + Qt + test app |

### Running individually

```bash
# Python (95% coverage)
pytest tests/unit_server/ -q
pytest tests/unit_server/ --cov=qt_commander --cov-report=term

# C++ via ctest (requires MSVC + Qt environment)
cd build/msvc && ctest --output-on-failure
```

### Coverage targets

| Scope | Target | Status |
|-------|--------|--------|
| Python (`qt_commander/`) | ≥95% overall | ✅ 95% |
| C++ main interfaces (43 public APIs) | 100% | ✅ |
| C++ overall | ≥95% functional | ✅ 96.5% |
| All test suites | 100% pass rate | ✅ 16/16 ctest |

## Project Structure

```
qt-commander/
├── qt_commander/              Python MCP server
│   ├── server.py            FastMCP app, 14 tools + 2 resources
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
│   │   ├── main.cpp         Entry point, argument parsing, exit codes 1-6
│   │   ├── injector.h       Public API declarations
│   │   ├── injector_win.cpp Win32 implementation (CreateRemoteThread, PE parser)
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
│   ├── unit_server/         Python unit tests (169)
│   ├── unit_injector/       C++ unit + E2E tests (~130)
│   ├── unit_library/        C++ library component tests (~36)
│   └── test-apps/           Minimal Qt test applications
│
├── scripts/
│   └── run_all_tests.py     Unified cross-platform test runner
│
└── CMakeLists.txt
```

## License

MIT
