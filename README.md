# qt-commander

MCP server for Qt application introspection and automation — analogous to
Playwright for the browser, but targeting native Qt widgets and QML scenes.

## Prerequisites

- C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+)
- CMake 3.16+
- Qt 5.15+ (Core, Gui, Widgets) or Qt 6.x
- Optional for QML support: Qt Quick, Qt QuickWidgets

## Quick Start

Build from any writable directory. All paths below use placeholder values;
replace with your actual toolchain locations.

### 1. MCP Server (no Qt dependency)

```sh
cmake -S /path/to/qt-commander -B /path/to/build/server \
    -DBUILD_SERVER=ON -DBUILD_LIBRARY=OFF \
    -G "Your Generator"
cmake --build /path/to/build/server
```

### 2. Injection Library

**With QML support:**
```sh
cmake -S /path/to/qt-commander -B /path/to/build/lib-qml \
    -DBUILD_SERVER=OFF -DBUILD_LIBRARY=ON \
    -DWITH_QML=ON -DQT_MAJOR_VERSION=5 \
    -DCMAKE_PREFIX_PATH=/path/to/qt/5.15.2/msvc2019_64 \
    -G "Your Generator"
cmake --build /path/to/build/lib-qml
```

**Without QML (pure QWidget):**
```sh
cmake -S /path/to/qt-commander -B /path/to/build/lib-noqml \
    -DBUILD_SERVER=OFF -DBUILD_LIBRARY=ON \
    -DWITH_QML=OFF -DQT_MAJOR_VERSION=5 \
    -DCMAKE_PREFIX_PATH=/path/to/qt/5.15.2/msvc2019_64 \
    -G "Your Generator"
cmake --build /path/to/build/lib-noqml
```

### 3. Tests

```sh
cmake -S /path/to/qt-commander -B /path/to/build/tests \
    -DBUILD_SERVER=OFF -DBUILD_LIBRARY=OFF -DBUILD_TESTS=ON \
    -DQT_MAJOR_VERSION=5 \
    -DCMAKE_PREFIX_PATH=/path/to/qt/5.15.2/msvc2019_64 \
    -G "Your Generator"
cmake --build /path/to/build/tests
ctest --test-dir /path/to/build/tests
```

### 4. Everything at once

```sh
cmake -S /path/to/qt-commander -B /path/to/build/all \
    -DBUILD_SERVER=ON -DBUILD_TESTS=ON -DWITH_QML=ON \
    -DQT_MAJOR_VERSION=5 \
    -DCMAKE_PREFIX_PATH=/path/to/qt/5.15.2/msvc2019_64 \
    -G "Your Generator"
cmake --build /path/to/build/all
ctest --test-dir /path/to/build/all
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SERVER` | ON | Build qt-commander MCP server executable |
| `BUILD_LIBRARY` | OFF | Build libqt-commander injection library (auto-ON when `BUILD_TESTS=ON` if Qt found) |
| `BUILD_TESTS` | ON | Build all test suites; automatically enables `BUILD_LIBRARY` |
| `WITH_QML` | ON | Enable QML/QQuick support in the library |
| `QT_MAJOR_VERSION` | 5 | Qt major version: `5` or `6` |

> **Note:** The library can be built with or without QML support.
> If the target Qt application is a pure `QWidget` app, build with
> `-DWITH_QML=OFF` to avoid pulling in Qt Quick and QuickWidgets
> dependencies.

## Project Structure

```
qt-commander/
├── CMakeLists.txt
├── README.md
├── docs/                  # Design specification
├── external/              # Third-party (nlohmann/json, header-only)
├── src/
│   ├── common/            # Shared: socket_utils, framing protocol
│   ├── server/            # qt-commander MCP server
│   │   ├── mcp/           #   Transport (stdio/HTTP), protocol
│   │   ├── session/       #   Session management
│   │   ├── inject/        #   Platform injection (Windows/Linux/macOS)
│   │   ├── process/       #   Qt process detection
│   │   └── build/         #   Library build manager
│   └── library/           # libqt-commander (injected)
│       ├── core/          #   UI scanner, event injector, element map,
│       │                  #   screenshot capture
│       ├── rpc/           #   TCP JSON-RPC server
│       ├── selector/      #   JSON query → element matching
│       └── protocol/      #   RPC method dispatch handler
├── tests/
│   ├── test-apps/         # Test applications for integration tests
│   │   ├── widget/        #   QWidget-only (always built, no QML dep)
│   │   └── qml/           #   Pure QML (built with WITH_QML=ON)
│   ├── unit_server/       # Server unit tests
│   ├── unit_library/      # Library unit tests (needs Qt)
│   └── integration/       # End-to-end integration test
└── tools/
    └── quick_inject.cpp   # Manual process injection + snapshot utility
```

## Key Design Decisions

- **Two-process model:** The MCP server (`qt-commander`) and the target Qt
  process are separate. The library (`libqt-commander`) is injected into the
  target at runtime.
- **Two-channel communication:** Control commands go over TCP loopback with
  a custom length-prefixed frame protocol. Large data (UI trees, screenshots)
  go through the filesystem via MCP Resources.
- **Threading:** The injected library runs an RPC worker thread that dispatches
  all Qt widget operations to the main thread via `Qt::QueuedConnection` with
  a 30-second timeout.
- **Element lifecycle:** UI elements get monotonically increasing IDs per
  snapshot. IDs are invalidated on each new snapshot — agents should call
  `qt_snapshot` first, then operate by element ID.
- **QML optional:** `WITH_QML=ON` pulls in Qt Quick/QuickWidgets and enables
  QQuickWindow/QQuickItem traversal. Set to `OFF` for QWidget-only targets
  to keep the library lean.
