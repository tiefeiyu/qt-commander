# qt-commander

MCP server for Qt application introspection and automation — analogous to
Playwright for the browser, but targeting native Qt widgets and QML scenes.

## Architecture

```
AI Agent → Python MCP Server (fastmcp) → qt-injector.exe → libqt-commander.dll
                  ↑ subprocess                    ↑ CreateRemoteThread
```

- **MCP Server** (`mcp_server/`): Python + fastmcp, handles MCP protocol, sessions, builds
- **Injector** (`src/injector/`): C++ standalone CLI, injects DLL into target process
- **Library** (`src/library/`): C++/Qt, injected into target for UI introspection

## Quick Start

```bash
pip install -e ./mcp_server
qt-commander-mcp --transport stdio  # Start MCP server on stdio
```

Or development mode:
```bash
pip install -e ./mcp_server
python -c "from mcp_server.server import main; main()"
```

## Building Native Components

The C++ injector and library are compiled on-demand via the `qt_build` MCP tool:

```
qt_build(qt_env="C:/Qt/5.15.2/msvc2019_64/bin/qtenv2.bat",
         vcvars_path="C:/.../vcvars64.bat", vcvars_args="amd64")
```

This compiles both targets into `.qt-commander/build/`.

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_INJECTOR` | ON | Build qt-injector standalone CLI |
| `BUILD_LIBRARY` | OFF | Build injection library (auto-ON with BUILD_TESTS) |
| `BUILD_TESTS` | ON | Build all test suites |
| `WITH_QML` | ON | Enable QML/QQuick support in the library |
| `QT_MAJOR_VERSION` | 5 | Qt major version (5 or 6) |

## Testing

Tests are organized in three tiers:

| Tier | Location | Language | Count | Dependencies |
|------|----------|----------|-------|-------------|
| Unit | `tests/unit_server/` | Python | 169 | `pip install pytest pytest-cov` |
| Unit | `tests/unit_injector/` | C++ | ~80 | g++ (MinGW) or MSVC + CMake |
| Integration | `tests/unit_injector/` | C++ | ~30 | g++ or MSVC, Qt 5.15+ |

### Python (169 tests, 95% coverage)

No compiler needed — pure Python with mocked TCP/subprocess.

```bash
pip install -e ./mcp_server
pip install pytest pytest-cov

# Run all tests
pytest tests/unit_server/ -v          # 169 tests
pytest tests/unit_server/ -q          # concise output

# Coverage report
pytest tests/unit_server/ --cov=mcp_server --cov-report=term
# Expected: TOTAL 582 29 95%
```

### C++ with g++ (no Qt needed)

MinGW-w64 with g++ 13+. Covers DI injector logic, PE parser, Win32ProcessOps, CLI.

```bash
# Build and run individual test suites
g++ -std=c++17 -static -o build/test_di.exe \
  tests/unit_injector/test_injector_logic.cpp src/injector/injector_di.cpp \
  -I src/injector -I src/common -I src/library -lws2_32 -lpsapi -lbcrypt
./build/test_di.exe                    # 35 tests — injector DI logic

g++ -std=c++17 -static -o build/test_pe.exe \
  tests/unit_injector/test_pe_real.cpp \
  -I src/injector -I src/common -I src/library -lws2_32 -lpsapi -lbcrypt
./build/test_pe.exe                    # 7 tests — PE parser vs kernel32.dll

g++ -std=c++17 -static -o build/test_int.exe \
  tests/unit_injector/test_cli_integration.cpp src/injector/injector_di.cpp \
  -I src/injector -I src/common -I src/library -lws2_32 -lpsapi -lbcrypt
./build/test_int.exe                   # 30 tests — Win32ProcessOps + CLI args

g++ -std=c++17 -static -o build/test_win32.exe \
  tests/unit_injector/test_win32_coverage.cpp \
  -I src/injector -I src/common -I src/library -lws2_32 -lpsapi -lbcrypt
./build/test_win32.exe                 # ~15 tests — injector_win.cpp helpers
```

### C++ with MSVC + Qt (full suite)

Requires Visual Studio 2022+, Qt 5.15+, CMake 3.16+.

```bash
# Setup environment
call "C:\...\vcvars64.bat"
call "C:\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat"

# Configure
cmake -B build/msvc -G Ninja \
  -DBUILD_INJECTOR=ON -DBUILD_LIBRARY=ON -DBUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build/msvc

# Run ctest (7 tests)
cd build/msvc && ctest --output-on-failure
# Expected: 100% tests passed

# Run Python tests too
pytest tests/unit_server/ --cov=mcp_server --cov-report=term
```

### E2E Integration (requires compiled injector + Qt test app)

Tests the full lifecycle: inject → authenticate → RPC → snapshot → shutdown.

```bash
# Ensure Qt is on PATH
set PATH=C:\Qt\5.15.2\msvc2019_64\bin;%PATH%

# Build E2E test runner
g++ -std=c++17 -static -o build/test_e2e.exe \
  tests/unit_injector/test_e2e.cpp -lws2_32

# Run (expects binaries in build/msvc/)
./build/test_e2e.exe                   # 12 tests — full lifecycle

# Exit code coverage
g++ -std=c++17 -static -o build/test_exit.exe \
  tests/unit_injector/test_e2e_exit_codes.cpp -lws2_32

./build/test_exit.exe                  # 2 tests — exit 3, exit 6
```

### C++ Coverage (gcovr)

```bash
pip install gcovr

# Build tests with --coverage flag, run them, then:
gcovr -r . --filter "src/injector/" --exclude ".*test.*" --print-summary
```

## Tools

| Tool | Description |
|------|-------------|
| `qt_list_processes` | List running Qt processes |
| `qt_attach` | Inject library + open session |
| `qt_detach` | Disconnect + optionally eject library |
| `qt_list_sessions` | List active sessions |
| `qt_build` | Compile injector + library |
| `qt_snapshot` | Full UI tree snapshot |
| `qt_find_element` | Find elements by query |
| `qt_get_property` | Read element property |
| `qt_set_property` | Write element property |
| `qt_call_method` | Invoke QObject method |
| `qt_screenshot` | Capture element/window screenshot |
| `qt_mouse_click` | Send mouse click event |
| `qt_keyboard_input` | Send keyboard input |
| `qt_focus` | Set focus on element |
