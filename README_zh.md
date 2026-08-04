# qt-commander

面向 Qt 应用程序内省与自动化的 MCP 服务器 — 相当于浏览器领域的
[Playwright](https://playwright.dev)，但作用于原生 Qt Widget 和 QML 界面。

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

## 快速开始

```bash
# 安装（自动拉取 fastmcp 等运行时依赖）
pip install -e .

# 启动 MCP 服务器
python -m qt_commander
```

> **AI Agent / MCP 客户端配置**：详见 [llms-install.md](llms-install.md)。

## MCP 工具列表

| 工具 | 说明 |
|------|------|
| `qt_list_processes` | 列出正在运行的 Qt 进程（跨平台，基于 psutil） |
| `qt_attach` | 注入库到目标进程并建立会话 |
| `qt_detach` | 断开会话，可选卸载注入库 |
| `qt_list_sessions` | 列出当前活跃会话 |
| `qt_detect_msvc_and_qt` | 自动探测本机可用的 MSVC 和 Qt 安装 |
| `qt_build` | 按需编译注入器和库 |
| `qt_snapshot` | 捕获完整 UI 元素树 |
| `qt_find_element` | 按类型、文本或属性查询查找元素 |
| `qt_get_property` | 读取 QObject 属性 |
| `qt_set_property` | 写入 QObject 属性 |
| `qt_call_method` | 调用 QObject 方法 |
| `qt_screenshot` | 截取指定元素或窗口的截图 |
| `qt_mouse_click` | 向 UI 元素发送鼠标点击（直接投递） |
| `qt_mouse_click_at` | 在指定窗口坐标处点击 — 走真实 Qt 输入管线（QPA），真实场景图/控件树命中测试，与人类点击完全一致 |
| `qt_mouse_click_region` | 点击元素屏幕区域中心 — 由真实命中测试决定实际落点（如 QML Rectangle 内的 MouseArea） |
| `qt_keyboard_input` | 发送键盘输入 |
| `qt_focus` | 将焦点设置到指定元素 |

## 测试

全部测试（C++ 单元 + E2E 套件、pytest、部署级预加载验证）从同一个
CMake 构建树运行：

```powershell
# 单一构建树（注入器 + 库 + 测试应用 + 全部测试）
cmake -S . -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_TESTS=ON -DWITH_QML=ON ^
  -DCMAKE_BUILD_TYPE=Release -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5

# 构建全部，然后一条命令跑完所有测试：
cmake --build build/msvc
ctest --test-dir build/msvc --output-on-failure
```

`ctest` 运行 20 个套件：14 个注入器 C++ 套件（含对 widget 测试应用的
真实注入 E2E）、4 个库 C++ 套件、完整 pytest 套件（`python_unit_tests`），
以及 E2E 预加载验证（`verify_preload`，标记为 `e2e`，需要 `.qt-commander/bin`
中的 `qt_build` 产物）。

常用子集：

```bash
pytest tests/ -q                                   # 仅 Python
ctest --test-dir build/msvc -LE e2e                # 跳过慢速 E2E
ctest --test-dir build/msvc -R "test_selector"     # 单个套件
```

### 测试矩阵（2026-08 实测状态）

| 套件 | 位置 | 语言 | 测试数 | 依赖 |
|------|------|------|--------|------|
| 服务端单元 | `tests/unit_server/` | Python | 245 | Python 3.10+ |
| 注入器单元 | `tests/unit_injector/` | C++ | 276 | MSVC |
| 库单元 | `tests/unit_library/` | C++ | 55 | MSVC + Qt |
| 注入 E2E | `tests/unit_injector/test_e2e*.cpp` | C++ | 16 | MSVC + Qt + 测试应用 |
| 预加载 E2E | `tests/verify_preload.py` | Python | 3 个场景 | qt_build 产物 |

## 项目结构

```
qt-commander/
├── qt_commander/            Python MCP 服务器
│   ├── server.py            FastMCP 应用，16 个工具 + 2 个资源
│   ├── session.py           会话/会话管理器，带 RPC 锁
│   ├── rpc_client.py        子进程注入器启动器
│   ├── builder.py           按需 MSVC 编译编排器
│   ├── process_detector.py  跨平台 Qt 进程发现
│   ├── environment_detector.py  MSVC/Qt 构建环境自动探测
│   ├── framing.py           4 字节大端长度前缀帧协议
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
│       ├── core/            UI 扫描器、事件注入器、截图、元素映射
│       ├── protocol/        JSON-RPC 处理器
│       ├── rpc/             TCP RPC 服务器
│       └── selector/        元素查询引擎
│
├── tests/
│   ├── unit_server/         Python 单元测试（245 个）
│   ├── unit_injector/       C++ 单元 + E2E 测试（276 个）
│   ├── unit_library/        C++ 库组件测试（55 个）
│   ├── verify_preload.py    E2E：依赖预加载场景 A/B/C
│   └── test-apps/           最小化 Qt 测试应用程序
│
└── CMakeLists.txt
```

## 许可证

MIT
