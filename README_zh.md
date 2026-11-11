# qt-commander

> 中文文档覆盖安装/连接/产物/状态/限制等主要章节，细节以英文 [README.md](README.md)
> 为准（例如 MCP 工具参数、错误码）。

面向 AI Agent 的 Qt UI 实时观察与操作工具。

qt-commander 把一个很小的运行时链接进 Qt 应用，在本机 JSON-RPC 上暴露
活的对象树，并通过 MCP server 驱动它。查询每次都在目标进程里实时求值，
没有 snapshot 文件，也没有陈旧的 UI 索引。

## 安装

> 第一个 PyPI 版本还在准备中。发布前请用下面的"源码安装"；`qt-commander`
> 命令行已经可在仓库内使用。

### 前置要求

- Python 3.10+ 和 [uv](https://docs.astral.sh/uv/)
- Qt 5 或 Qt 6 的目标程序。当前实测 Qt 5.15.2 与 Qt 6.8.3；理论上兼容
  其它 Qt 5.x/6.x，但尚未逐个验证。
- Windows、Linux 或 macOS（macOS 仅 linked 与启动时注入，见下）
- Python 只用于 MCP server；目标进程不需要 Python 或 gdb

### 方式 1：包 + 预编译运行时（发布后可用）

```bash
pip install qt-commander-mcp
qt-commander doctor            # 体检：Qt kit、工具链、产物、descriptor
qt-commander install-artifacts # 下载本机 OS/架构对应的注入库
```

`install-artifacts` 会校验 `SHA256SUMS`，并拒绝越界的归档成员；不需要 C++
编译器。产物根目录可用 `QT_COMMANDER_ARTIFACTS` 指定。

### 方式 2：源码安装

```bash
git clone https://github.com/tiefeiyu/qt-commander.git
cd qt-commander
uv sync
uv run qt-commander doctor
```

### 2. 注册到 MCP 客户端

Claude Code：

```bash
claude mcp add qt-commander -- uv run --project /absolute/path/to/qt-commander qt-commander-mcp
```

Claude Desktop / Cursor / 其它 JSON 客户端：

```json
{
  "mcpServers": {
    "qt-commander": {
      "command": "uv",
      "args": [
        "run",
        "--project",
        "/absolute/path/to/qt-commander",
        "qt-commander-mcp"
      ]
    }
  }
}
```

路径用本机绝对路径（Windows 可用 `D:/Develop/qt-commander`）。改完配置后重启
客户端；`qt_status` 应该能列出 descriptor 或活动会话。

### 3. 连接目标

- **Linked runtime**：构建/接入一次应用或测试程序（见
  [构建 linked 运行时](#构建-linked-运行时)）；它会写出 descriptor。
- **已运行目标**：调用 `qt_attach(pid=<pid>, mode="injection")`；不知道 PID
  时先用 `qt_processes`，Qt 版本/工具链未知时用 `qt_target_info`，必要时构建
  匹配的 injector 和 runtime。
- **还没启动（Linux / macOS）**：`qt_launch(program, library=...)` 用
  `LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` 把运行时预载进去再启动，等 descriptor
  出现后直接 attach；接受 macOS `.app`。这是 macOS 唯一的注入方式——系统不允许
  往已运行的硬化/公证进程里注入。Windows 没有对应机制，因此**不注册**该工具。

通常的第一次 agent 交互：

```text
qt_status -> qt_attach -> qt_windows -> qt_query -> qt_act
```

## 当前状态

- 本仓库实测 Qt 5.15.2（MinGW/MSVC）与 Qt 6.8.3（MinGW/MSVC），
  linked 与 injection 均验证；Ubuntu 24.04 + Qt 5.15.2 gcc_64（XCB）的
  linked、99 个 Python 测试和 MCP stdio E2E 也已验证。
- linked mode：应用在 `QApplication` 之后调用 `qt_commander::start()`，
  写出 rendezvous descriptor。
- 原生 injection（Windows/Linux）：`qt_attach(pid=..., mode="injection")`
  调用 C++ `qtc-inject` 加载 `qt_commander_inject.dll/.so`，在目标 GUI 线程
  启动同一套 in-process server；`auto` 在无 live descriptor 时自动注入。
  Python 只是 MCP 编排层，目标机不需要 Python 或 gdb。Linux ptrace 仍需
  Yama 权限（让注入器成为目标的父进程，或设置
  `kernel.yama.ptrace_scope=0`）。可用 `QT_COMMANDER_INJECTOR` 覆盖原生
  可执行路径。
- macOS：linked 与启动时注入（`qt_launch` + `DYLD_INSERT_LIBRARIES`）已在 CI
  验证（arm64 + Qt 6.8.3）；不支持往已运行的硬化/公证进程注入。
- 版本号只有一个来源：git tag。`setuptools-scm` 从 tag 推导包版本，CMake 把同一个
  tag 编进运行时（`library_version`），两边不会漂移；artifact 的 `manifest.json`
  另记 `source{version, commit, describe}` 便于追溯。详见 `docs/versioning.md`。
- 已注册 22 个 MCP 工具（Windows 上 21 个，没有 `qt_launch`）。当前可用：
  `status`、`target_info`、`processes`、`environments`、`build`、`build_status`、
  `attach`、`detach`、`launch`（Linux/macOS）、`windows`、`query`、`items`、`act`、
  `batch`、`wait`、`screenshot`、`get`、`set`、`call`、`methods`、`source`、
  `task`。`source` 只做有界扫描
  （默认 120 ms / 2 万条目预算，结果带 `search{roots,visited,truncated}`），
  未命中返回 `E_NO_DEBUGINFO`，预算耗尽返回 `E_SOURCE_TIMEOUT`；命中时可按
  `snippet_lines`/`max_lines` 返回代码片段。
- 目标端所有数据请求进同一条 FIFO 队列、由目标的事件循环执行，所以槽卡在
  `exec()` 里不再占用 socket：读请求、`act` 和 `qt_task` 在同一条连接上照常工作。
  `qt_act`/`qt_call`/`qt_set` 支持 `wait_ms`（默认 1000/3000/3000 ms），超时返回
  `{"state":"running","task_id":...}`，结果存入任务表，之后用
  `qt_task(task_id, wait_ms=...)` 取回。这类调用**已经派发**，不要重试；
  `ping` 会返回 `queued`/`inflight`/`active` 便于诊断。
- `qt_build` 是后台 job：`wait_s` 内没完成会返回 `status:"running"` + `job_id`，
  用 `qt_build_status` 查询；相同参数的重复调用复用一个 job。构建目录被其它源码
  占用时会自动隔离到 `_<hash>` 后缀，不再覆盖别人的 build tree。
- `qt_processes` 三级判定：live descriptor → exe 邻接 Qt 运行时 → **进程已加载的
  模块**（`psutil.memory_maps`，带 2 s 预算与缓存）。每条结果带
  `evidence: descriptor|adjacent|module`；`QT_COMMANDER_PROCESS_MODULES=0` 可关闭
  模块扫描。
- `qt_query` 的 `where` 支持 `inherits`（沿 metaobject 继承链匹配，例如
  `{"inherits":"QTableView"}` 能命中 `QTableWidget`）；`compact=true` 省略默认值
  字段以省 token；隐藏剪枝会通过 `hidden_pruned` 与
  `hidden_matched` + hint 暴露出来，不再静默 `count:0`。
- `qt_attach(artifact_dir=...)` 可在多个 artifact 匹配时指定目录；artifact 选择
  优先“精确版本 > 唯一带版本 > 报歧义”。
- `items` 已覆盖 `QTabWidget`/`QTabBar`、`QMenuBar`/`QMenu`、`QHeaderView`、
  `QTableView`、`QTreeView`、`QListView`、`QComboBox`、`QScrollBar`、`QSlider`、
  `QSpinBox`、可勾选的 `QGroupBox`、`QDockWidget` 标题按钮、`QSplitter`
  手柄、`QToolBar` action；custom/proxy model 通过 item-view adapter 工作。
- `QQuickWidget` 下的 QML 深层对象会出现在 `query` 树中，带
  `qml_type`/`qml_id`/`objectName`/局部 rect；其 `vqml` ID 可用于
  `qt_get`、`qt_act`、`qt_screenshot` 裁剪和 `qt_source`。
- 纯 QML 应用（QQuickWindow，无 QWidget 顶层）也可直接使用：
  `qt_windows/qt_query/qt_items/qt_get/qt_set/qt_act`（QML 点击/按键/输入）和
  `qt_screenshot`（窗口或元素裁剪）。
- `qt_query` 的 `limit` 是软截断：返回部分树并附带
  `count/total/limit/truncated`，不再直接报错。
- `qt_call` 只调用 Qt 元对象方法（slot / `Q_INVOKABLE`），QWidget 和 QML item
  都支持；`QPushButton::setText` 这类属性 setter 不是方法，请用 `qt_set`。
  非 void 返回值放在 `result`；对象返回值是 `{class, id}`，可直接复用为 target；
  `qt_methods` 可列出方法、参数类型和返回类型（默认隐藏 Qt `_q_` 内部方法）；
  用 `signature` 消歧重载，枚举参数可传整数。
- `qt_target_info` 在 `qt_build` 前检查目标进程的 PE/ELF 架构、完整 Qt 版本
  （例如 5.15.2）、构建类型和工具链；它不注入、不启动目标。
  `qt_processes` 可发现正在运行的 Qt 目标并返回 attach 提示，不执行注入。
- `qt_source` 会自动搜索附近的源码树（包括祖先目录下的 `src`），也支持
  `source_dir` 指定 qrc/嵌入式 QML 的源码根；找到时返回 `file/line/url`。
- `qt_screenshot(annotate=...)` 支持 `annotate_offset` 分页；legend 的 `n`
  跨页连续。
- `qt_batch` 每步必须是 `{"act": ...}`、`{"wait": ...}`、`{"set": ...}` 或
  `{"get": ...}` 之一，按顺序完成写入/读取/恢复（失败不自动回滚）。整批共享
  `timeout_ms`（默认 30000，上限 60000）：`wait` 会被裁剪到剩余时间，超预算时
  返回已完成步骤 + `cause="batch_timeout"`，不会让整个调用被客户端掐断；
  `max_steps`（默认 50）限制步数，等待预算在派发前校验。
- `qt_act(target=<窗口 id>, action="minimize"|"restore")` 可最小化/恢复
  QWindow/QQuickWindow；restore 保持原几何。
  `action="set_geometry"` + `geometry={x,y,w,h}` 返回实际 `resolved.rect`
  和 `applied`。
- `qt_act` 支持 click/dblclick/type/focus/key/scroll/hover/drag，以及声明式
  `expect`（where/props/gone）；`type` 对容器目标自动寻找可编辑子控件，
  找不到返回 `E_NOT_EDITABLE`；虚拟 item 支持 click/dblclick。
- target 可直接用字符串作为 QML `qml_id`，也可组合
  `qml_id`/`text`/`class`/`objectName`（含子节点文本）。文本命中会自动折叠到
  最近的可点击祖先；例如按钮内标签会点击按钮。隐藏目标默认排除，除非传
  `include_hidden=true`，否则返回 `E_HIDDEN`。
- MCP 层截图 PNG 限制约 6 MiB；超限会自动降低宽度重截并返回
  `auto_downscaled`，避免大图导致连接断开。

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

## 注入产物

`qt_build` 只写入一个根目录：

```text
artifacts/<os>-<arch>/<compiler>-<version>/qt-<qt-version>-<debug|release>/
  manifest.json
  bin/qtc-inject.exe
  lib/qt_commander_inject.dll   # Linux 为 libqt_commander_inject.so
```

例如：

```text
artifacts/windows-x64/msvc-19.29/qt-5.15.2-debug/
artifacts/windows-x64/mingw-gcc-13.1/qt-5.15.2-release/
artifacts/linux-x64/gcc-13.3/qt-6.8.3-release/
```

`qt_attach(mode="injection")` 按目标的 OS/架构、编译器家族、Qt 版本和
Qt Debug/Release 匹配 `artifacts/` 下的 manifest；不扫描 `build/`，也不按
修改时间挑最新文件。只有显式覆盖时才使用
`QT_COMMANDER_INJECTOR` / `QT_COMMANDER_INJECT_DLL`。

产物的三种来源：

1. `qt-commander install-artifacts`——从 GitHub Release 下载
   `qt-commander-artifacts-<os>-<arch>-*.zip`，校验 `SHA256SUMS` 后解包；
   `--from-dir` / `--from-url` 用于离线。
2. `qt_build(for_pid=<pid>)`——按活目标的 Qt kit 现编。
3. `scripts/package_artifacts.py`——手工 CMake 构建后的打包入口。

装进 site-packages 的 wheel 写不了包目录，`install-artifacts` 会回落到按用户
的目录；用 `QT_COMMANDER_ARTIFACTS` 可以固定位置。从 git checkout 构建的产物
会在 `manifest.json` 里记 `source{version, commit, describe}`，
`qt_target_info` 和 `qt-commander doctor --pid` 会打印它。

## 启动 MCP server

```bash
uv sync
uv run --project . qt-commander-mcp
```

如果 Windows 报 `qt-commander-mcp.exe` 被占用，先停掉正在运行的
Claude/MCP 进程，再重新执行 `uv run`。

客户端用 `qt_attach(descriptor=...)` 指向 descriptor，或在启动 server 前
设置 `QT_COMMANDER_RENDEZVOUS`。对已运行的 Windows Qt 应用，使用
`qt_attach(pid=<pid>, mode="injection")`；DLL 路径可用
`QT_COMMANDER_INJECT_DLL` 覆盖。

MCP server 自带 `instructions`，覆盖启动、目标选择、歧义/隐藏处理和破坏性
操作安全；每个工具也有带 schema 的说明。

## 已知限制

- Qt Quick Controls 的 `Popup` 根对象（例如 `indicatorPopup`）可能无法通过
  `qml_id` 查到；请改用弹层内的文字或子节点查询。渲染在主窗口 Overlay 里的
  `Popup` 不是独立窗口，`qt_windows` 仍只返回主窗口；独立的 `Qt.Popup` 窗口
  会出现在 `qt_windows` 中，并带 `popup: true`。
- 修改 C++ runtime 代码后，必须重建 artifact 并重启目标进程。只重启 MCP
  server 不会更新已经加载到目标进程里的旧 runtime。
- macOS 不能注入到已在运行的进程（系统层面无等价机制）；只能 linked 或
  `qt_launch` 启动时注入。SIP 保护的路径（`/usr`、`/System`、`/bin`、`/sbin`）
  和硬化运行时（需同 Team 签名 + `disable-library-validation`）会拒绝插入。
- macOS 屏幕级截图需要"屏幕录制"权限（`QScreen::grabWindow`）；widget 级截图
  （`QWidget::grab`）不需要。无权限时返回 `E_PERMISSION` 并附操作指引。

## 故障排查

- 先跑 `qt-commander doctor --json`（加 `--pid <pid>` 可检查单个目标）。它会列出
  Python/依赖、Qt kit、工具链、已装产物、live descriptor，以及 `qt_attach` 会选
  哪个产物、是否需要重启。
- `qt_status` 没有 descriptor：先启动 linked 目标，或用 `qt_processes`
  找到 PID 后再 `qt_attach(pid=..., mode="injection")`。
- 桌面客户端找不到 `uv`：把 `command` 配成 `uv` 的绝对路径。
- `qt-commander-mcp.exe` 被占用：先停掉旧的 Claude/MCP 进程，再重启客户端。
- 注入失败：先看 `error.reason`。`injector_already_loaded` 和
  `qt_runtime_flavor_mismatch` 表示必须重启目标；`artifact_missing` 表示跑
  `qt-commander install-artifacts` 或 `qt_build(for_pid=<pid>)`。`error.details`
  里有目标快照、选中的产物、已加载的注入库和期望产物。

多窗口时先调用 `qt_windows`，把窗口 `id` 传给 `qt_screenshot`。不传 target
且存在多个可见 top-level 窗口时，截图会明确报错，不会静默选一个；QMenu/QToolTip
等 popup 不计入歧义判断。`qt_windows(include_hidden=true)` 可列出隐藏的
QQuickWindow。`qt_query(scope=...)` 可以限定到单个窗口/控件子树；
不传 `props` 时节点默认带 `text/visible/enabled`；`where` 同时接受
`object_name` 和 `objectName` 别名；字符串支持通配符或
`contains/startsWith/regex/equals` 匹配器。QAction/菜单项是虚拟项，用
`qt_items(owner=menu, where=...)` 查询，不在 QWidget query 树中。

## 测试

```bash
uv run pytest tests/unit_server -q
```

当前：**99 passed**（Qt5 MinGW/MSVC 测试应用单独构建）。

纯 QML 回归程序与 linked 使用同样的 Qt 参数构建：

```bash
cmake -S tests/test-apps/qmlwindow -B build/qmlwindow-mingw5 -G Ninja \
  -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5
cmake --build build/qmlwindow-mingw5
```

端到端 / benchmark：

```bash
# linked smoke（先启动 tests/test-apps/linked）
uv run python scripts/linked_smoke.py
uv run python scripts/mcp_linked_smoke.py
uv run python scripts/window_regression.py
uv run python scripts/qml_window_test.py

# 原生 injection smoke（传入目标 pid）
uv run python scripts/injection_smoke.py <pid> --mode injection

# 异步任务队列：pending -> task_id -> qt_task、嵌套 exec、batch、soak
cmake -S tests/test-apps/modal -B build/modal-mingw5 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5
cmake --build build/modal-mingw5
uv run python scripts/task_regression.py

# P0 benchmark
uv run python scripts/p0_large.py --label large-7000
uv run python scripts/p0_matrix.py --exe build/matrix-mingw5/qt-matrix-test.exe
```

## 目录

```
qt_commander/        Python MCP server、protocol、client、session、ops、注入适配器
src/                 C++ 运行时（linked 库 + injection DLL）
src/injector/        原生 C++ qtc-inject（Windows remote thread / Linux ptrace）
tests/unit_server/   Python 单元测试
tests/test-apps/     linked / large / injected / matrix / qmlwindow 测试应用
scripts/             smoke、注入与 P0 benchmark 脚本
docs/design.md       当前设计记录
docs/agent-eval.md   Agent 验收任务与指标
docs/benchmarks/     large-7000.json 与 matrix.json
CODE_STANDARDS.md    代码与评审规范
```

## License

MIT。见 [LICENSE](LICENSE)。

`v0.8.0` 及更早的版本按 `MIT AND Anti-996` 发布，各自保留当时的条款。变更理由见
[docs/release/LICENSE-NOTES.md](docs/release/LICENSE-NOTES.md)；反 996 文本作为非约束性声明保留在
[docs/community/anti-996.md](docs/community/anti-996.md)。
