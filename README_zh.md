# qt-commander

面向 Qt 应用程序内省与自动化的 MCP 服务器 — 相当于浏览器领域的
[Playwright](https://playwright.dev)，但作用于原生 Qt Widget 和 QML 界面。

![Qt 5.15](https://img.shields.io/badge/Qt-5.15-blue)
![Qt 6.8](https://img.shields.io/badge/Qt-6.8-brightgreen)
![MSVC](https://img.shields.io/badge/MSVC-supported-blue)
![MinGW](https://img.shields.io/badge/MinGW-supported-blue)
![Platform](https://img.shields.io/badge/Windows-supported-important)
![License](https://img.shields.io/badge/License-MIT-green)

## 为什么选择 qt-commander

AI Agent（Claude、Cursor 等）可以像 Playwright 驱动网页一样**驱动原生
Qt 应用程序**：

- **无需修改源码** — 库被注入到*正在运行*的进程中；任何 Qt 应用
  （自己开发的或第三方）都可以直接操作。
- **两种 UI 栈全支持** — QWidget 与 QML/Qt Quick，Qt 5.15 与 Qt 6.8，
  MSVC 与 MinGW。
- **Agent 能拿到什么** — 完整 UI 快照（几何、z 序、可见性、透明度、
  属性）；按人眼可见性做遮挡修剪的视图；按文本/类型/属性查找元素；
  真实输入管线点击、打字、快捷键、拖拽。
- **开箱即用** — `uv run python -m qt_commander`；注入器与库按需
  针对检测到的任意 Qt kit 编译。

## 快速开始

```bash
# 通过 uv 启动 MCP 服务器（无需全局安装 Python 包——uv 依据
# pyproject.toml / uv.lock 自动管理隔离环境）
uv run python -m qt_commander
```

需要 [uv](https://docs.astral.sh/uv) 与 Python 3.10+。

### MCP 客户端配置

**Claude Code** — 加入 `.mcp.json`（或用户级 MCP 配置）：

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

**Cursor / 其他 MCP 客户端** — 命令格式相同；服务器走 stdio MCP 协议，
无需其他配置。完整安装指南（含移除旧 pip 安装方式）见
[llms-install.md](llms-install.md)。

## 架构

```
┌──────────┐     stdio      ┌──────────────┐   子进程调用    ┌──────────────┐
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

| 组件 | 路径 | 语言 | 职责 |
|------|------|------|------|
| MCP 服务器 | `qt_commander/` | Python | 协议桥接、会话管理、按需编译 |
| 注入器 CLI | `src/injector/` | C++ | 将库加载到目标进程的独立可执行文件 |
| 注入库 | `src/library/` | C++/Qt | 进程内引擎，负责 UI 内省、操作、截图 |
| 公共模块 | `src/common/` | C++ | 帧协议、TCP socket 工具 |

### 工作流程

1. **AI Agent** 通过 stdio 发送 MCP 工具调用（如 `qt_snapshot`）。
2. **MCP Server** 作为子进程启动 `qt-injector.exe`，传入目标进程 PID。
3. **qt-injector** 通过 `CreateRemoteThread` + `LoadLibraryW` 将
   `libqt-commander.dll` 注入目标 Qt 进程。注入前会**预加载库的传递依赖闭包**
   （目标程序未链接的 Qt DLL，例如纯 QML 应用的 Qt5Widgets）— 无需在
   目标程序旁手动拷贝任何 Qt DLL。随后执行令牌认证握手，
   将库的 TCP 端口号输出到 stdout。
4. **MCP Server** 通过 TCP 连接到注入库，使用 4 字节大端长度前缀帧协议
   转发 RPC 调用（快照、点击、输入等）。

## MCP 工具列表

| 工具 | 说明 |
|------|------|
| `qt_list_processes` | 列出正在运行的 Qt 进程（跨平台，基于 psutil） |
| `qt_attach` | 注入库到目标进程并建立会话 |
| `qt_detach` | 断开会话，可选卸载注入库 |
| `qt_list_sessions` | 列出当前活跃会话 |
| `qt_detect_msvc_and_qt` | 自动探测本机可用的 MSVC、MinGW 工具链和 Qt 安装 |
| `qt_build` | 按需编译注入器和库 — `toolchain` 选择 `msvc`（vcvars + qtenv bat）或 `mingw`（MinGW bin 目录 + Qt bin 目录） |
| `qt_snapshot` | 捕获 UI 元素树 — `detail` 选择属性档位：`core`（几何/可见性/文本，无属性）、`extended`（常用交互状态）、`full`（全部 Q_PROPERTY） |
| `qt_prune_snapshot` | 遮挡求解：剔除被更高 z 序不透明元素完全盖住的节点（同 z 按创建顺序，后创建的遮挡先创建的；子元素绘制在父之上、父不遮挡子），部分遮挡节点标注 `visible_ratio`，输出精简快照 |
| `qt_find_element` | 按类型、文本或属性查询查找元素 |
| `qt_get_property` | 读取 QObject 属性 |
| `qt_set_property` | 写入 QObject 属性 |
| `qt_call_method` | 调用 QObject 方法 |
| `qt_screenshot` | 截取指定元素或窗口的截图 |
| `qt_mouse_click` | 向 UI 元素发送鼠标点击（直接投递） |
| `qt_mouse_click_at` | 在指定窗口坐标处点击 — 走真实 Qt 输入管线（QPA），真实场景图/控件树命中测试，与人类点击完全一致 |
| `qt_mouse_click_region` | 点击元素屏幕区域中心 — 由真实命中测试决定实际落点（如 QML Rectangle 内的 MouseArea） |
| `qt_mouse_press` | 在元素上按下鼠标按键（不松开） |
| `qt_mouse_release` | 释放之前按下的鼠标按键（完成一次点击或拖动） |
| `qt_mouse_move` | 将指针移动到元素局部坐标处（拖动 = press → move → release） |
| `qt_keyboard_input` | 发送键盘输入（键入文本，可带按住的修饰键） |
| `qt_key_combo` | 发送快捷键，如 `Ctrl+C`、`Ctrl+Shift+A`（带修饰键的真实按下/释放序列） |
| `qt_focus` | 将焦点设置到指定元素 |

## 测试

全部测试（C++ 单元 + E2E 套件、pytest、部署级预加载验证）从同一个
CMake 构建树运行：

```powershell
# 单一构建树（注入器 + 库 + 测试应用 + 全部测试）。
# Qt5 与 Qt6 均支持，按需选择：
#   Qt5 MSVC：-DQT_MAJOR_VERSION=5 -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5
#   Qt6 MSVC：-DQT_MAJOR_VERSION=6 -DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6
#   Qt6 MinGW：同上，但 Qt6_DIR=C:/Qt/6.8.3/mingw_64/lib/cmake/Qt6
#              且 MinGW 工具链在 PATH 上（见下方「MinGW」）
cmake -S . -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_TESTS=ON -DWITH_QML=ON ^
  -DCMAKE_BUILD_TYPE=Release -DQT_MAJOR_VERSION=6 ^
  -DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6

# 构建全部，然后一条命令跑完所有测试：
cmake --build build/msvc
ctest --test-dir build/msvc --output-on-failure
```

`verify_preload`（部署级 E2E）会自动检测部署的 `libqt-commander.dll`
所属的 Qt 主版本**和工具链类型**（msvc/mingw）并验证对应 DLL 集合——
连 windeployqt 也会跟随部署的工具链——因此同一脚本能从任一构建树
验证 Qt5/Qt6 × msvc/mingw 的全部组合。

## MinGW

MinGW 构建完全受支持（Qt5 与 Qt6 MinGW kit）。使用 `qt_build` 并传
`toolchain="mingw"`：`vcvars_path` 传 MinGW 工具链的 bin 目录，
`qt_env` 传 kit 的 qtenv2.bat（MinGW Qt kit 与 MSVC kit 一样自带
qtenv2.bat）。`qt_detect_msvc_and_qt` 会报告 MinGW 工具链
（`mingw_toolchains`），并为每个 Qt kit 标注 `kit`（"msvc"/"mingw"）。

注意事项：

- **编译器版本**：Qt 5 官方 MinGW kit 配套 GCC 8.1，其 libstdc++ 头
  无法编译 `std::filesystem`（8.3 才修复）；Qt 5 也应使用 GCC ≥ 9
  工具链（如 Qt 自带的 `mingw1310_64`）。`qt_build` 会显式固定编译器
  （`-DCMAKE_C/CXX_COMPILER`），PATH 上的其他 gcc（如 Strawberry Perl
  工具链）不会被误选。
- **运行时 DLL**：MinGW 可执行文件需要 `libgcc_s_seh-1.dll`、
  `libstdc++-6.dll`、`libwinpthread-1.dll` 位于其旁。构建会自动部署
  编译器自身的运行时（Qt kit 自带的旧版运行时缺少新符号），
  `verify_preload` 也会让部署应用的运行时与库保持一致。
- **工具链匹配**：注入库、部署目录与目标应用必须使用同一 Qt kit
  （全部 MSVC 或全部 MinGW）——混用会在一个进程里加载两套 Qt 模块，
  破坏预加载闭包。

`ctest` 运行 19 个套件：14 个注入器 C++ 套件（含对 widget 测试应用的
3 个真实注入 E2E 套件）、3 个库 C++ 套件、完整 pytest 套件
（`python_unit_tests`），以及 E2E 预加载验证（`verify_preload`，标记为
`e2e`，需要 `.qt-commander/bin` 中的 `qt_build` 产物）。

E2E 套件会自动把工作目录锚定到自身所在的构建树
（`test_util.h::chdir_to_exe_dir`），因此从仓库根目录直接运行与通过
`ctest` 运行的结果完全一致。

常用子集：

```bash
pytest tests/ -q                                   # 仅 Python
ctest --test-dir build/msvc -LE e2e                # 跳过慢速 E2E
ctest --test-dir build/msvc -R "test_selector"     # 单个套件
```

### 测试矩阵（2026-08 实测状态）

| 套件 | 位置 | 语言 | 测试数 | 依赖 |
|------|------|------|--------|------|
| 服务端单元 | `tests/unit_server/` | Python | 254 | Python 3.10+ |
| 注入器单元 | `tests/unit_injector/` | C++ | 326 | MSVC |
| 库单元 | `tests/unit_library/` | C++ | 43 | MSVC + Qt |
| 注入 E2E | `tests/unit_injector/test_e2e*.cpp` | C++ | 48 | MSVC + Qt + 测试应用 |
| 预加载 E2E | `tests/verify_preload.py` | Python | 3 个场景 | qt_build 产物 |

## 项目结构

```
qt-commander/
├── qt_commander/            Python MCP 服务器
│   ├── server.py            FastMCP 应用，22 个工具 + 2 个资源
│   ├── session.py           会话/会话管理器，带 RPC 锁
│   ├── rpc_client.py        子进程注入器启动器
│   ├── builder.py           按需 MSVC 编译编排器
│   ├── process_detector.py  跨平台 Qt 进程发现
│   ├── environment_detector.py  MSVC/Qt 构建环境自动探测
│   ├── framing.py           4 字节大端长度前缀帧协议
│   ├── occlusion.py         快照遮挡求解（剔除被覆盖元素，标注可见比例）
│   └── errors.py            MCP 错误码注册表
│
├── src/
│   ├── common/              共享 C++ 工具
│   │   ├── framing.h        帧协议（header-only）
│   │   ├── socket_utils.h   TCP 抽象层
│   │   └── socket_utils.cpp
│   ├── injector/            独立注入 CLI
│   │   ├── main.cpp         入口点，参数解析，--list-deps，退出码 1-6
│   │   ├── injector.h       公开 API 声明
│   │   ├── injector_win.cpp Win32 实现（CreateRemoteThread、PE 导入表
│   │   │                    解析、依赖闭包预加载）
│   │   ├── injector_di.cpp  依赖注入变体（基于 IProcessOps，完全可测试）
│   │   └── os_ops.h         IProcessOps / MockProcessOps / Win32ProcessOps
│   └── library/             注入 DLL
│       ├── entry_win.cpp    DllMain / Windows 入口
│       ├── api.h            InitParams 握手结构体（1024 字节）
│       ├── compat_qt.h      Qt5/Qt6 兼容宏
│       ├── core/            UI 扫描器、事件注入器、截图、元素映射
│       ├── rpc/             TCP RPC 服务器（JSON-RPC 处理器）
│       └── selector/        元素查询引擎
│
├── tests/
│   ├── unit_server/         Python 单元测试（254 个）
│   ├── unit_injector/       C++ 单元 + E2E 测试（326 个）
│   ├── unit_library/        C++ 库组件测试（43 个）
│   ├── verify_preload.py    E2E：依赖预加载场景 A/B/C
│   └── test-apps/           最小化 Qt 测试应用程序
│
└── CMakeLists.txt
```

## 许可证

[MIT](LICENSE) — 可自由使用、修改、分发，可集成到商业项目，保留署名即可。
