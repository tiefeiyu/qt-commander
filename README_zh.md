# qt-commander

面向 AI Agent 的 Qt UI 实时观察与操作工具。

qt-commander 把一个很小的运行时链接进 Qt 应用，在本机 JSON-RPC 上暴露
活的对象树，并通过 MCP server 驱动它。查询每次都在目标进程里实时求值，
没有 snapshot 文件，也没有陈旧的 UI 索引。

## 当前状态

- 本仓库实测 Qt 5.15.2（MinGW/MSVC）与 Qt 6.8.3（MinGW）；
  Ubuntu 24.04 + Qt 5.15.2 gcc_64（XCB）的 linked、72 个 Python 测试和
  MCP stdio E2E 也已验证。Qt 6 MSVC 未单独构建。
- linked mode：应用在 `QApplication` 之后调用 `qt_commander::start()`，
  写出 rendezvous descriptor。
- Windows injection：`qt_attach(pid=..., mode="injection")` 注入
  `qt_commander_inject.dll`，在目标 GUI 线程启动同一套 in-process server；
  `auto` 在无 live descriptor 时自动注入。
- 已注册 15 个 MCP 工具。当前可用：
  `status`、`environments`、`build`、`attach`、`detach`、`query`、`items`、
  `act`、`batch`、`wait`、`screenshot`、`get`、`set`、`call`、`source`。
  `source` 先按 objectName 搜索 `.ui`/`.qml`，未命中才返回身份信息和
  `E_NO_DEBUGINFO`。
- `items` 已覆盖 `QTabWidget`/`QTabBar`、`QMenuBar`/`QMenu`、`QHeaderView`、
  `QTableView`、`QTreeView`、`QListView`、`QComboBox`、`QScrollBar`、`QSlider`；
  更复杂的 custom-model provider 尚未覆盖。
- `qt_act` 支持 click/dblclick/type/focus/key/scroll/hover/drag，以及声明式
  `expect`（where/props/gone）；虚拟 item 支持 click/dblclick。

## 构建 linked 运行时

```bash
cmake -S tests/test-apps/linked -B build/linked -G Ninja \
  -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5 \
  -DCMAKE_CXX_COMPILER=/path/to/mingw/g++.exe
cmake --build build/linked
```

MSVC 可以直接调用 MCP 的 `qt_build`：它通过 `vswhere` 发现 Visual Studio，
自动加载 `vcvars64.bat`，也支持显式 `vcvars_path`。

Linux 使用官方 Qt kit 时，如果 Qt5GuiConfigExtras 报
`/usr/include/libdrm` 下找不到 `GL/gl.h`，在 configure 时加
`-D_qt5gui_OPENGL_INCLUDE_DIR=/usr/include`。

Windows 上把 Qt `bin` 和 MinGW runtime 放进 `PATH`，并把 `QT_PLUGIN_PATH`
指向 Qt 的 `plugins`，然后：

```powershell
$env:QT_COMMANDER_RENDEZVOUS = "$PWD\.test-workspace\rendezvous"
.\build\linked\qt-linked-test.exe
```

应用会把 `descriptor-<pid>.json` 写进该目录（一进程一份；POSIX 下仅属主
可读写）。

## 启动 MCP server

```bash
uv sync
uv run --project . qt-commander-mcp
```

客户端用 `qt_attach(descriptor=...)` 指向 descriptor，或在启动 server 前
设置 `QT_COMMANDER_RENDEZVOUS`。对已运行的 Windows Qt 应用，使用
`qt_attach(pid=<pid>, mode="injection")`；DLL 路径可用
`QT_COMMANDER_INJECT_DLL` 覆盖。

## 测试

```bash
uv run pytest tests/unit_server -q
```

当前：**72 passed**（Qt5 MinGW/MSVC 测试应用单独构建）。

端到端 / benchmark：

```bash
# linked smoke（先启动 tests/test-apps/linked）
uv run python scripts/linked_smoke.py
uv run python scripts/mcp_linked_smoke.py

# Windows injection smoke（传入目标 pid）
uv run python scripts/injection_smoke.py <pid> --mode injection

# P0 benchmark
uv run python scripts/p0_large.py --label large-7000
uv run python scripts/p0_matrix.py --exe build/matrix-mingw5/qt-matrix-test.exe
```

## 目录

```
qt_commander/        Python MCP server、protocol、client、session、ops、注入器
src/                 C++ 运行时（linked 库 + injection DLL）
tests/unit_server/   Python 单元测试
tests/test-apps/     linked / large / injected / matrix 测试应用
scripts/             smoke、注入与 P0 benchmark 脚本
docs/design.md       当前设计记录
docs/benchmarks/     large-7000.json 与 matrix.json
CODE_STANDARDS.md    代码与评审规范
```

## License

MIT AND Anti 996。见 `LICENSE`、`LICENSE.MIT`、`LICENSE.996ICU`。
