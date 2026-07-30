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

```bash
# Python tests (34 tests, no external deps)
pytest tests/unit_server/ -v

# C++ tests (require CMake + compiler)
cmake -B build -DBUILD_INJECTOR=ON -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
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
