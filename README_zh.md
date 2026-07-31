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
   `libqt-commander.dll` 注入目标 Qt 进程，执行令牌认证握手后，
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

## 编译

注入器和库需要 **Visual Studio 2022+**、**Qt 5.15+** 和 **CMake 3.16+**。
可以手动编译，也可以通过 `qt_build` MCP 工具按需编译。

### 手动编译

```powershell
# 配置环境
cmd /c "C:\...\vcvars64.bat" amd64
cmd /c "C:\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat"

# 配置 & 构建
cmake -B build/msvc -G Ninja ^
  -DBUILD_INJECTOR=ON -DBUILD_LIBRARY=ON ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/msvc
```

### 按需编译（通过 MCP 工具）

```python
qt_build(
    qt_env="C:/Qt/5.15.2/msvc2019_64/bin/qtenv2.bat",
    vcvars_path="C:/.../vcvars64.bat",
    vcvars_args="amd64"
)
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_INJECTOR` | ON | 构建 `qt-injector.exe` |
| `BUILD_LIBRARY` | OFF | 构建 `libqt-commander.dll`（启用 `BUILD_TESTS` 时自动开启） |
| `BUILD_TESTS` | ON | 构建所有测试套件 |
| `WITH_QML` | ON | 启用 QML/QQuick 支持 |
| `QT_MAJOR_VERSION` | 5 | Qt 主版本号（5 或 6） |

## MCP 工具列表

| 工具 | 说明 |
|------|------|
| `qt_list_processes` | 列出正在运行的 Qt 进程（跨平台，基于 psutil） |
| `qt_attach` | 注入库到目标进程并建立会话 |
| `qt_detach` | 断开会话，可选卸载注入库 |
| `qt_list_sessions` | 列出当前活跃会话 |
| `qt_build` | 按需编译注入器和库 |
| `qt_snapshot` | 捕获完整 UI 元素树 |
| `qt_find_element` | 按类型、文本或属性查询查找元素 |
| `qt_get_property` | 读取 QObject 属性 |
| `qt_set_property` | 写入 QObject 属性 |
| `qt_call_method` | 调用 QObject 方法 |
| `qt_screenshot` | 截取指定元素或窗口的截图 |
| `qt_mouse_click` | 发送鼠标点击事件 |
| `qt_keyboard_input` | 发送键盘输入 |
| `qt_focus` | 将焦点设置到指定元素 |

## 测试

```bash
# 仅 Python — 无需编译器
python scripts/run_all_tests.py

# 完整套件 — Python + C++（需要 MSVC + Qt）
python scripts/run_all_tests.py ^
  --vcvars "C:\...\vcvars64.bat" ^
  --qt-env "C:\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat"

# 快速模式 — 跳过 CMake 配置步骤
python scripts/run_all_tests.py --quick --vcvars "..." --qt-env "..."
```

### 测试矩阵

| 套件 | 位置 | 语言 | 测试数 | 依赖 |
|------|------|------|--------|------|
| 服务端单元 | `tests/unit_server/` | Python | 169 | Python 3.10+ |
| 注入器单元 | `tests/unit_injector/` | C++ | ~100 | MSVC |
| 库单元 | `tests/unit_library/` | C++ | ~36 | MSVC + Qt |
| E2E 集成 | `tests/unit_injector/test_e2e*.cpp` | C++ | ~30 | MSVC + Qt + 测试应用 |

### 单独运行

```bash
# Python（95% 覆盖率）
pytest tests/unit_server/ -q
pytest tests/unit_server/ --cov=qt_commander --cov-report=term

# C++ 通过 ctest（需要 MSVC + Qt 环境）
cd build/msvc && ctest --output-on-failure
```

### 覆盖率目标

| 范围 | 目标 | 状态 |
|------|------|------|
| Python (`qt_commander/`) | 整体 ≥95% | ✅ 95% |
| C++ 主要接口（43 个公开 API） | 100% | ✅ |
| C++ 整体 | 函数级 ≥95% | ✅ 96.5% |
| 全部测试套件 | 100% 通过率 | ✅ 16/16 ctest |

## 项目结构

```
qt-commander/
├── qt_commander/              Python MCP 服务器
│   ├── server.py            FastMCP 应用，14 个工具 + 2 个资源
│   ├── session.py           会话/会话管理器，带 RPC 锁
│   ├── rpc_client.py        子进程注入器启动器
│   ├── builder.py           按需 MSVC 编译编排器
│   ├── process_detector.py  跨平台 Qt 进程发现
│   ├── framing.py           4 字节大端长度前缀帧协议
│   └── errors.py            MCP 错误码注册表
│
├── src/
│   ├── common/              共享 C++ 工具
│   │   ├── framing.h        帧协议（header-only）
│   │   ├── socket_utils.h   TCP 抽象层
│   │   └── socket_utils.cpp
│   ├── injector/            独立注入 CLI
│   │   ├── main.cpp         入口点，参数解析，退出码 1-6
│   │   ├── injector.h       公开 API 声明
│   │   ├── injector_win.cpp Win32 实现（CreateRemoteThread、PE 解析器）
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
│   ├── unit_server/         Python 单元测试（169 个）
│   ├── unit_injector/       C++ 单元 + E2E 测试（~130 个）
│   ├── unit_library/        C++ 库组件测试（~36 个）
│   └── test-apps/           最小化 Qt 测试应用程序
│
├── scripts/
│   └── run_all_tests.py     统一跨平台测试运行器
│
└── CMakeLists.txt
```

## 许可证

MIT
