# qt-commander Python MCP Refactor Design

**Date:** 2026-07-30
**Status:** Review
**Context:** 将 C++ MCP Server 替换为 Python + fastmcp 实现，注入器拆分为独立 CLI

---

## 1. 动机

### 1.1 当前问题

- C++ MCP Server 手写 JSON-RPC 2.0 协议（~500 行）、HTTP/SSE 传输层（~400 行），维护成本高
- MCP 协议持续演进（prompts、sampling、Elicitation 等），每次都要手动跟进
- 缺乏官方 SDK 生态支持，无法利用 Python 丰富的测试和异步生态

### 1.2 目标

- **降低维护成本：** 用 `fastmcp` 替代手写协议层，MCP 协议演进由 SDK 吸收
- **利用生态：** 获得 prompts、sampling、OAuth 等高级 MCP 特性
- **模块化拆分：** DLL 注入器独立为 CLI，Python MCP 通过 subprocess 调用

---

## 2. 架构概览

```
AI Agent                        Python MCP Server              注入库
   │                                  │                          │
   ├─ MCP 协议 (stdio/HTTP) ────────►│                          │
   │   fastmcp 处理                   │                          │
   │                                  ├─ subprocess ──────────►  │
   │                                  │   qt-injector.exe        │
   │                                  │                         │
   │                                  ├─ TCP + 帧协议 ─────────►│
   │                                  │   127.0.0.1             libqt-commander
   │                                  │                          (Qt 目标进程内)
```

### 2.1 三个组件

| 组件 | 语言 | 职责 |
|------|------|------|
| `mcp_server/` | Python + fastmcp | MCP 协议处理、会话管理、TCP RPC 客户端、构建编排、工具调度 |
| `src/injector/` | C++ | 独立 CLI：注入 DLL → 初始化 → 返回端口+token |
| `src/library/` | C++ / Qt | **不变**，注入目标进程的内省库 |

### 2.2 删除的代码

| 目录/文件 | 原因 |
|-----------|------|
| `src/server/mcp/` | MCP 协议处理由 fastmcp 替代 |
| `src/server/session/` | 会话管理由 Python 实现 |
| `src/server/process/` | 进程检测由 Python 实现 |
| `src/server/build/` | 构建编排由 Python 实现 |
| `src/server/main.cpp` | Server 入口由 Python 替代 |
| `tools/quick_inject.cpp` | 功能并入正式注入器 |
| `tests/unit_server/` (C++) | 由 `tests/unit_server/` (Python) 替代 |
| `tests/integration/test_integration.cpp` | 由 Python 集成测试替代 |

### 2.3 保留的代码

| 目录/文件 | 处理 |
|-----------|------|
| `src/library/` | 需要两处适配：(1) 帧协议头从 6 字节改为 4 字节 `framing.h`；(2) auth handler 接受 64 hex token（`token` 字段已有 128 字节空间，仅运行时比较逻辑变更） |
| `src/common/` | 精简：`framing.cpp` → 删除、`framing.h` → header-only（injector 和 library 共用）；`socket_utils.*` 保留供 injector 编译使用 |

---

## 3. 帧协议简化

**旧协议（6 字节头）：** `[0xCC][0x01][4-byte BE length][payload]`
**新协议（4 字节头）：** `[4-byte BE length][payload]`

- localhost TCP 无数据损坏，magic 校验无必要
- 注入库和 MCP Server 由同一 SDK 分发，版本号校验多余
- C++ 端改为 **纯头文件 inline 函数**，零依赖，~20 行
- Python 端 `struct.pack("!I")` / `struct.unpack("!I")`，~3 行
- **最大 frame 大小：16 MB**。长度 > 16 MB 拒绝分配，返回错误后关闭连接。长度 = 0 同理
- **无 desync 恢复：** 如果连接流失步（断电/OS bug），对方看到的第一个 4 字节被当作长度解析。由于有 16 MB 上限，极大垃圾长度会被拒绝。如果垃圾值恰好落入 [1, 16 MB] 范围，则误读一个大数据块后自行恢复

```cpp
// framing.h — header-only, 供 library 和 injector 共用
inline std::vector<uint8_t> frame_encode(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(4 + len);
    uint32_t be = htonl(static_cast<uint32_t>(len));
    memcpy(out.data(), &be, 4);
    memcpy(out.data() + 4, data, len);
    return out;
}
```

### 3.1 RPC 帧内协议

帧承载的是 **JSON-RPC 2.0** 格式的消息（与 MCP 协议规范一致，但与 AI Agent 无关）：

```json
// 请求（由 MCP Server 发送到注入库）
{"jsonrpc":"2.0","method":"qt.snapshot","params":{"detail":"extended"},"id":1}

// 认证（第一次连接时必须发送）
{"jsonrpc":"2.0","method":"qt.authenticate","params":{"token":"<64 hex>"},"id":1}

// 成功响应
{"jsonrpc":"2.0","result":{"elements":[...]},"id":1}

// 错误响应
{"jsonrpc":"2.0","error":{"code":-32000,"message":"element destroyed","data":{"code":1001}},"id":1}
```

- **id** 由 Python RPC 客户端递增管理，用于匹配请求和响应
- **认证** 必须在第一个帧发送，5 秒内未认证则库关闭连接
- **发送/接收超时**：`tcp_set_recv_timeout(30s)`、`tcp_set_send_timeout(30s)`
- **TCP keepalive**：`SO_KEEPALIVE` + 5s 间隔，检测库/Server 断开

## 4. MCP 初始握手

`fastmcp` 自动处理 `initialize` 握手，本 Server 注册如下能力：

```json
// initialize 响应（由 fastmcp 生成）
{
  "protocolVersion": "2024-11-05",
  "capabilities": {
    "tools": {},
    "resources": {
      "subscribe": true,
      "listChanged": true
    },
    "logging": {}
  },
  "serverInfo": {
    "name": "qt-commander",
    "version": "1.0.0"
  }
}
```

- `tools` — 注册 14 个工具（见第 7 节）
- `resources` — 通过 `@mcp.resource()` 装饰器注册，订阅 snapshot/screenshot 变化
- `resources.listChanged` — 在 `qt_snapshot` 或 `qt_screenshot` 创建新文件后发送 `notifications/resources/list_changed`
- `logging` — `logging/setLevel` 接受但不实际控制日志级别

---

## 5. 独立注入器

### 4.1 接口

```bash
qt-injector <pid> <library_path> <port_file_path>
# stdout: {"port": 12345, "token": "a3f9..."}
# exit 0: 成功
# exit 非 0: 失败，stderr 输出错误
```

### 4.2 执行流程

```
1. OpenProcess(PID)
2. VirtualAllocEx → WriteProcessMemory(DLL 路径) → CreateRemoteThread(LoadLibraryW)
3. WaitForSingleObject(30s) → GetExitCodeThread → 拿到 DLL base
4. 解析 DLL PE 导出表 → 找 qt_commander_init RVA
5. VirtualAllocEx → WriteProcessMemory(InitParams)
6. CreateRemoteThread(qt_commander_init, InitParams 地址)
7. WaitForSingleObject(10s)
8. 轮询 port_file_path（指数退避: 50ms → ... → 3.2s）
9. 读到 port + token → stdout JSON → exit 0
```

### 4.3 设计要点

- **注入器不持有连接**，读完 port 文件即退出
- **token 由注入器生成**（CSPRNG, 64 hex 字符），不再由 Server 端生成
- **注入器无状态**，每次调用独立进程
- InitParams 保持 v1 布局（1024 bytes），仅 token 字段填充方式变化

### 4.4 错误码

| Exit Code | 含义 |
|-----------|------|
| 0 | 成功 |
| 1 | OpenProcess 失败（权限/进程不存在） |
| 2 | LoadLibraryW 失败（架构不匹配/DLL 路径错误） |
| 3 | qt_commander_init 超时或返回非 0 |
| 4 | port 文件超时未就绪 |
| 5 | Token 不匹配（port 文件中的 token ≠ InitParams 中写入的 token） |

### 4.5 eject 命令

```bash
qt-injector --eject <pid> <library_path>
# 重新枚举目标进程中的模块列表，找到匹配的 DLL base
# 调用 CreateRemoteThread(FreeLibrary, dllBase)
# exit 0: 成功，exit 非 0: 失败
```

无需存储 HMODULE 状态 — 每次调用独立通过 `EnumProcessModules` 查找模块。

### 4.6 源码结构

```
src/injector/
├── CMakeLists.txt
├── main.cpp                # CLI 入口，参数解析
├── injector.h              # 接口声明
├── injector_win.cpp        # Windows
├── injector_linux.cpp      # Linux
└── injector_macos.cpp      # macOS
```

依赖 `src/common/framing.h`（header-only，通过 include path 引用）和 `src/common/socket_utils.*`。

---

## 6. 构建系统

### 5.1 双模式源码定位

```
开发阶段                              发布阶段
────────                              ────────
qt-commander/                         site-packages/qt_commander_mcp/
├── mcp_server/                       ├── server.py
├── src/                              └── native/
│   ├── injector/                         ├── src/injector/
│   ├── library/                          ├── src/library/
│   └── common/                           └── src/common/
└── CMakeLists.txt
```

Server 启动时检测优先级：
1. 环境变量 `QT_COMMANDER_NATIVE_SRC`（开发）
2. 包内路径 `<package>/native/`（发布）
3. `qt_build` 工具参数可显式覆盖

### 5.2 `qt_build` 工具

```
qt_build (MCP tool)
  参数:
    vcvars_path:    string  (Windows 必须) — vcvars32.bat 或 vcvars64.bat
    vcvars_args:    string  (可选) — 如 "amd64" / "x86"
    qt_env:         string  (必须) — qtenv2.bat 路径
    build_type:     string  (可选, 默认 "Release")
    qt_major:       integer (可选, 默认 5)
    generator:      string  (可选, 自动检测)
  返回:
    { injector_path, library_path, qt_version, arch }
```

### 5.3 构建流程

```
1. 确定 native 源码路径
2. 生成 .bat 脚本（Windows）:
     @call <vcvars_path> <vcvars_args>
     @call <qt_env>
     cmake -S <native>/src/injector -B <build>/injector
       -G <generator> -DCMAKE_BUILD_TYPE=<build_type>
     cmake --build <build>/injector
     cmake -S <native>/src/library -B <build>/library
       -DQT_MAJOR_VERSION=<qt_major> -DBUILD_SERVER=OFF
       -DCMAKE_BUILD_TYPE=<build_type>
     cmake --build <build>/library
3. subprocess.run(脚本)
4. 返回产物路径
```

### 5.4 懒编译

工作空间布局：

```
.qt-commander/
├── build/
│   ├── injector/
│   │   └── build/          # CMake 产物
│   └── library/
│       └── build/          # CMake 产物
└── sessions/
    └── <session_id>/
        ├── port.txt
        ├── snapshots/
        └── screenshots/
```

内部状态机：

```python
class BuildState:
    NOT_BUILT = "not_built"     # 从未构建，或产物文件不存在
    BUILDING = "building"       # 加锁避免并发编译
    BUILT = "built"             # 产物文件存在且有效

    # Server 重启时：检查产物文件是否存在 → 存在则置为 BUILT，不存在则置为 NOT_BUILT
    def detect_on_startup(self): ...
```

Server 重启时 BUILDING 状态自然丢失（进程空间状态），不会出现永久卡住的问题。

当 `qt_attach` 检测到 `NOT_BUILT` 时，通过抛出异常返回 **MCP JSON-RPC 错误**（而非普通返回值）：

```json
// JSON-RPC Error Response（由 fastmcp 通过 raise 生成）
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32000,
    "message": "Build required — compile the injector and library first using qt_build",
    "data": {
      "code": 2001,
      "hint": {
        "required_params": ["vcvars_path", "qt_env"],
        "optional_params": ["vcvars_args", "build_type", "qt_major", "generator"]
      }
    }
  },
  "id": "req-xxx"
}
```

Python 实现方式：

```python
class BuildRequiredError(Exception):
    pass

# 在 qt_attach 中:
if not build_state.is_built():
    raise BuildRequiredError(
        "需要先构建注入器和库。请使用 qt_build 工具。"
    )
# fastmcp 自动转为 -32000 错误；附加 data.code 通过自定义错误处理器注入
```

### 5.5 不编译发布

C++ 代码随 SDK 以源码形式分发，使用时通过 `qt_build` 在目标机器上按需编译。这保证：
- 架构自动匹配目标机器
- Qt 版本天然兼容（链接目标机器上的 Qt）
- 无预编译二进制分发问题

---

## 7. Python MCP Server 结构

```
mcp_server/
├── pyproject.toml              # 包定义
├── server.py                   # FastMCP 实例 + 14 工具注册
├── session.py                  # Session + SessionManager
├── rpc_client.py               # TCP 帧协议 RPC 客户端
├── builder.py                  # qt_build 逻辑
├── framing.py                  # 帧协议 Python 端 (4-byte BE)
└── native/                     # 发布时嵌入的 C++ 源码
    └── src/
        ├── injector/
        ├── library/
        └── common/
```

### 6.1 依赖

```toml
[project]
dependencies = [
    "fastmcp>=2.0",
    "psutil>=5.0",
]
```

仅 `fastmcp` 一个依赖（fastmcp 自身依赖官方 `mcp` SDK）。

### 6.2 工具注册（示例）

```python
from fastmcp import FastMCP, Context
from session import SessionManager, check_build

mcp = FastMCP("qt-commander")
sessions = SessionManager()

@mcp.tool()
async def qt_list_processes() -> str: ...

@mcp.tool()
async def qt_attach(ctx: Context, pid: int) -> str:
    build_err = check_build()
    if build_err:
        return build_err
    ...

@mcp.tool()
async def qt_build(vcvars_path: str, qt_env: str, ...) -> str: ...

# ... 其余 10 个工具: detach, list_sessions,
#     snapshot, find_element, get_property, set_property,
#     call_method, screenshot, mouse_click, keyboard_input, focus
```

### 6.3 Session 生命周期

```python
class Session:
    id: str                    # 12位 [a-z0-9]
    pid: int
    lib_path: Path             # 注入的库路径（qt_detach 时 eject 需要）
    port: int
    token: str                 # 64 hex
    reader: StreamReader        # asyncio
    writer: StreamWriter
    _rpc_lock: asyncio.Lock    # 序列化 send_rpc 调用 ← 关键
    snapshot_count: int = 0

    async def connect(self, port: int, token: str): ...
    async def disconnect(self): ...
    async def send_rpc(self, method: str, params: dict) -> dict:
        async with self._rpc_lock:  # 保证帧不交错
            ...
            # 发送请求帧
            # 接收响应帧
            # 匹配 id
            ...

class SessionManager:
    _sessions: dict[str, Session]
    _pid_to_session: dict[int, str]
    _lock: asyncio.Lock          # 保护字典操作

    async def create(self, pid: int, lib_path: Path) -> Session: ...
    async def destroy(self, session_id: str, purge: bool = False): ...
    async def get(self, session_id: str) -> Session | None: ...
```

### 6.5 RPC 通信流程

```
Python Server                                      libqt-commander
     │                                                  │
     ├─ subprocess: qt-injector <pid> <lib> <port_file>│
     │   timeout: 60s                                   │
     │   create_subprocess_exec                         │
     │   await proc.wait()                              │
     │   if exit ≠ 0 → raise InjectionError             │
     │                                                  │
     ├─ 解析 stdout JSON → port + token                  │
     ├─ connect_with_retry(port, max_attempts=3) ─────►│
     │   (500ms between retries, total 1.5s)            │
     │                                                  │
     ├─ send_frame(auth request) ─────────────────────►│
     │                                                  ├─ 验证 token
     │                                                  │   非 auth 首消息 → close
     │                                                  │   5s 内未认证 → close
     │                                                  │   token 错误 → error + close
     ├─ ◄── auth OK ───────────────────────────────────│
     │                                                  │
     ├─ send_frame("qt.snapshot") ────────────────────►│
     │   await wait_for(reader.read, timeout=30)        │
     │                                                  ├─ 截图 → 写文件
     ├─ ◄── {"result":{...}} ──────────────────────────│
     │                                                  │
     ├─ 读资源文件 → 返回给 Agent                         │
```

- **subprocess timeout**：注入器最长运行 ~55s（30s LoadLibrary + 10s init + ~15s 轮询），subprocess 设置 60s 超时
- **connect retry**：库写 port 文件和调 `listen()` 之间有微小窗口，Python 重试最多 3 次
- **RPC timeout**：每个 `send_rpc` 受 `asyncio.wait_for(30s)` 保护。超时 → raise `RpcTimeoutError`

### 6.6 进程检测

新增 `mcp_server/process_detector.py` 模块，负责 `qt_list_processes` 的实现：

| 平台 | 方法 |
|------|------|
| **Windows** | `psutil.process_iter()` 枚举进程 + 检查加载模块是否包含 `Qt5Core.dll` / `Qt6Core.dll` / debug 变体 |
| **Linux** | 读取 `/proc/<pid>/maps`，匹配 `libQt*Core.so*` |
| **macOS** | `psutil` + `vmmap` 检查 `QtCore.framework` |

```python
# process_detector.py
import psutil

def list_qt_processes() -> list[dict]:
    """返回: [{"pid": 1234, "name": "myapp.exe", "title": "My App",
               "qt_version": "5.15.2", "arch": "x64", "bitness": 64}, ...]"""
    ...
```

依赖 `psutil` 加入 `pyproject.toml`。

### 6.7 资源注册

通过 `@mcp.resource()` 装饰器将 session 文件暴露为 MCP Resources：

```python
@mcp.resource("qt-commander://sessions/{session_id}/snapshots/{filename}")
async def read_snapshot(session_id: str, filename: str) -> str:
    """返回 snapshot JSON 文本"""
    ...

@mcp.resource("qt-commander://sessions/{session_id}/screenshots/{filename}")
async def read_screenshot(session_id: str, filename: str) -> bytes:
    """返回 PNG 图片二进制"""
    ...
```

- URI scheme 使用 `qt-commander://`（而非旧的 `session://`），避免与其他 MCP server 冲突
- `resources/listChanged` 能力在 `initialize` 中声明，每次 `qt_snapshot` 或 `qt_screenshot` 创建新文件后发送通知

---

## 8. 工具变更

共 **14 个工具**（新增 1 个 + 语义变更 2 个 + 不变 11 个）。

### 7.1 新增工具

| 工具 | 说明 |
|------|------|
| `qt_build` | 新：在目标机器上编译注入器和库（替代了旧设计中的 `qt_build_library`，现在同时编译两个 CMake 目标） |

### 7.2 语义变更

| 工具 | 变更 |
|------|------|
| `qt_attach` | 参数不变，但若未构建则抛出 `BuildRequiredError`（MCP error code -32000, data.code 2001）而非执行注入 |
| `qt_detach` | 增加 eject 步骤（调用 `qt-injector --eject <pid> <lib_path>`） |

### 7.3 不变的工具

以下 11 个工具业务语义不变：

`qt_list_processes`, `qt_list_sessions`, `qt_snapshot`, `qt_find_element`,
`qt_get_property`, `qt_set_property`, `qt_call_method`, `qt_screenshot`,
`qt_mouse_click`, `qt_keyboard_input`, `qt_focus`

### 7.4 与旧版工具面差异

旧设计（2026-07-29 第 5 节）定义了约 **28 个工具**，新版精简为 **14 个**。具体变更：

| 旧工具（合并/删除） | 新替代 | 理由 |
|---------------------|--------|------|
| `qt_mouse_press`, `qt_mouse_release`, `qt_mouse_dblclick`, `qt_mouse_move`, `qt_mouse_wheel` → 删除 | 使用 `qt_call_method` + `qt_mouse_click` | 旧实现全是占位符（`session_manager.cpp` 中的 PLACEHOLDER 注释），从未实质实现。在实现这些工具前先删除，按需添加 |
| `qt_key_press`, `qt_key_release`, `qt_type_text`, `qt_key_combo` → 删除 | 保留 `qt_keyboard_input`（重命名自 `qt_type_text`） | 同上——占位符不能调用，先删后可量力加回 |
| `qt_touch_press`, `qt_touch_move`, `qt_touch_release` → 删除 | 无 | 触摸模拟为 deferred 特性 |
| `qt_clear_focus`, `qt_context_menu` → 删除 | 使用 `qt_call_method` | 当前为占位符 |
| `qt_build_library` → 重命名 | `qt_build` | 现在同时编译注入器和库 |

**迁移原则：** 已从旧 design spec 中移除的工具均为 **PLACEHOLDER**（从未实现），删除不影响功能。后续若要加回个别工具，各自有明确的 inputSchema 和实现路径。

### 7.5 错误码映射

旧版错误码体系（`-32000` + `data.code` 子码）保持不变，Python 通过自定义异常类映射：

```python
class QtCommanderError(Exception):
    """data.code 1001-2010 → MCP -32000"""
    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message

# 子类
class ElementDestroyedError(QtCommanderError):  # 1001
class ElementStaleError(QtCommanderError):       # 1002
class SessionNotFoundError(QtCommanderError):    # -32602
class RpcTimeoutError(QtCommanderError):         # 2003
# ...
```

fastmcp 通过 `mcp.custom_error_handler` 将 `QtCommanderError` 映射到 MCP JSON-RPC error 格式。

---

## 9. 测试策略

```
tests/
├── unit_server/                  # pytest（新，替代原 C++ unit_server）
│   ├── test_framing.py           # 帧协议编解码、边界条件（长度=0、>16MB 拒绝）
│   ├── test_session.py           # Session CRUD、PID 去重、会话并发安全（asyncio.Lock）
│   ├── test_builder.py           # 构建脚本生成、BuildState 状态机、并发构建锁
│   ├── test_process_detector.py  # psutil mock：进程枚举、Qt DLL 检测、平台差异
│   ├── test_rpc_client.py        # RPC 客户端（虚假 asyncio stream）：认证、超时、错误码映射
│   ├── test_tools.py             # fastmcp TestClient：14 个工具、build_required 错误、参数校验
│   ├── test_error_codes.py       # 验证所有 data.code (1001-2010) 正确映射到 MCP error
│   └── test_auth.py              # Token 生成格式、port 文件解析、认证失败场景
│
├── unit_injector/                # catch2（新）
│   ├── test_injector_cli.cpp     # CLI 参数解析、exit code 映射
│   ├── test_pe_parser.cpp        # PE 导出表解析（给定测试 DLL 文件）
│   ├── test_port_poll.cpp        # 端口文件轮询逻辑（指数退避算法）
│   └── test_framing.cpp          # C++ 端 framing.h 编解码（与 Python 端交叉验证）
│
├── unit_library/                 # QTestLib（不变 — 现有文件保留，新增以下）：
│   ├── test_selector.cpp         # 已存在
│   ├── test_element_map.cpp      # 已存在
│   ├── test_handler.cpp          # 已存在（依赖 handler_test_stubs）
│   ├── test_ui_scanner.cpp       # 新增：Widget/QML 树遍历、周期检测、截断
│   ├── test_event_injector.cpp   # 新增：鼠标/键盘/触摸事件注入、修饰符
│   └── test_screenshot.cpp       # 新增：Widget grab、QQuickItem grabToImage async
│
└── integration/
    ├── test_integration.py       # pytest（新，替代 C++ 版）
    │   # 端到端: attach → snapshot → find → property r/w → click → type → screenshot → detach
    ├── test_concurrency.py       # 并发测试：PID 去重、TCP 断开恢复、并发双会话、detach 期间 snapshot
    └── test_error_paths.py       # 错误路径：stale ID、destroyed element、invisible/disabled、zero-size
        # 前提: 二进制文件已编译好（不测试构建流程）
```

### 8.1 测试取舍

- **不测构建流程** — 构建依赖外部工具链（MSVC + vcvars + Qt），不属于自动化测试范围
- **`test_builder.py`** 仅测试脚本生成逻辑（生成的 batch 脚本内容正确），不真正执行编译
- **集成测试** 假设注入器和库已手动编译好，只测端到端 RPC 流程

### 8.2 并发测试（对应旧 spec 13.4 节全部保留）

| # | 场景 | 预期 |
|---|------|------|
| 1 | `qt_snapshot` + `qt_click` 交错调用 | 第二次请求因 `_rpc_lock` 排队，不交错帧 |
| 2 | 两次 `qt_snapshot` 并发 | 依次执行，第二次成功 |
| 3 | 同一 PID 两次 `qt_attach` | 第二次 reject（PID 去重） |
| 4 | TCP 断开恢复 | Server 检测连接丢失，标记 session 僵尸 |
| 5 | 库检测 Server 死亡 | TCP keepalive 触发，库清理 RPC 线程 |
| 6 | 双 session 独立操作 | 相互不影响 |
| 7 | `qt_detach` 期间有 `qt_snapshot` 进行中 | 优雅关闭 |
| 8 | Server 在 recv 期间被杀死 | 库检测断连，停止接受请求 |

### 8.3 集成测试前提

- 二进制编译由开发者手动完成或 CI 预先构建
- 集成测试只验证：进程间通信、工具业务逻辑、会话生命周期、错误处理路径
- 环境变量 `QT_COMMANDER_DLL`、`QT_INJECTOR_EXE`、`QT_WIDGET_TEST_APP` 指定二进制路径

### 8.4 handler_test_stubs 清理

现有的 `tests/unit_library/handler_test_stubs.cpp` 将所有 UiScanner/EventInjector/Screenshot 函数返回为 no-op。一旦 `test_ui_scanner.cpp`、`test_event_injector.cpp`、`test_screenshot.cpp` 作为独立测试实现，stubs 中的模拟逻辑将移至各个测试文件的 fixture 中。

---

## 10. 与旧版兼容性

### 9.1 帧协议不兼容

新旧帧协议头不同（6 bytes vs 4 bytes），**新 Server 无法连接旧库，旧 Server 无法连接新库**。

但这不是问题：注入库和 Server 由同一 SDK 发布，版本始终一致。

### 9.2 InitParams 兼容

`InitParams` 结构体保持 v1 布局（1024 bytes），**不兼容旧版 token 长度**（旧版 32 hex → 新版 64 hex）。新版注入器需要匹配新版库的 entry point 实现。

**已知限制：** `InitParams` 路径字段使用 `char[260]` 缓冲区。在 Linux/macOS 上 `PATH_MAX` 可达 4096，超长路径可能截断。此问题从旧设计继承，将在库 v2 结构体中解决。

### 9.3 资源 URI 方案

**新方案：** `qt-commander://sessions/<session_id>/<type>/<filename>`

**旧方案：** `session://<session_id>/<type>/<filename>`

更换为 `qt-commander://` scheme 以避免与其他 MCP server 的 URI 命名空间冲突。路径结构（`sessions/{id}/{type}/{filename}`）保持不变。

---

## 11. 项目目录对比

```
旧结构                              新结构
────────                              ────────
src/                                  src/
├── server/        ← 删除             ├── injector/       ← 新（独立 CLI）
│   ├── mcp/                          │   ├── main.cpp
│   ├── session/                      │   └── injector.*
│   ├── inject/    → 移入 injector/   ├── library/        ← 修改（帧协议）
│   ├── process/   ← 删除             │   └── ...
│   └── build/     ← 删除             └── common/         ← 精简
├── library/       ← 修改（帧协议）       ├── framing.h    ← header-only
├── common/        ← 精简                 └── socket_utils.*
│   ├── framing.cpp → 删除
│   └── framing.h   → header-only

tools/                                 mcp_server/         ← 新
├── quick_inject.cpp  ← 删除           ├── pyproject.toml
                                       ├── server.py
tests/                                 ├── session.py
├── unit_server/    ← pytest           ├── rpc_client.py
├── unit_library/   ← 不变             ├── builder.py
└── integration/    ← pytest           ├── framing.py
                                       └── native/
   新增: unit_injector/                    └── src/...
```

## 12. 已知前置 bug 修复

当前代码库中存在一个 **InitParams 结构体不匹配**问题（重构前即存在）：

- `src/library/api.h` 定义了标准布局：`workspace_path[256]`, `session_id[13]`, `token[65]`, `port_file_path[256]`, `reserved[426]`（总计 1024 字节）
- `src/server/inject/injector_win.cpp` 定义了**不同的本地布局**：`workspace_path[260]`, `session_id[128]`, `token[128]`, `port_file_path[260]`（~776 字节）

这导致 injector 写入 776 字节布局而 library 读取 1024 字节布局——偏移量不匹配。**重构时必须修复**，将两个定义统一为 `api.h` 的标准布局。

## 13. 安全边界

### 13.1 PID 验证

`qt-injector` 必须在注入前验证目标进程是 Qt 进程（防御性检查）：

```
1. OpenProcess → EnumProcessModules → GetModuleBaseNameW
2. 检查模块列表是否包含 Qt5Core*.dll / Qt6Core*.dll
3. 不匹配 → exit code 6（非 Qt 进程）
```

### 13.2 DLL 路径验证

- `library_path` 必须规范化为绝对路径
- 必须位于预期的构建输出目录（`.qt-commander/build/library/`）或已知安全位置
- 拒绝包含 `..`、`~` 或符号链接指向外部目录的路径

### 13.3 资源 URI 路径遍历防护

`@mcp.resource()` 中的 `session_id` 和 `filename` 必须验证：

```python
import re
SESSION_ID_PATTERN = re.compile(r'^[a-z0-9]{12}$')
SAFE_FILENAME_PATTERN = re.compile(r'^[a-zA-Z0-9_.-]+$')

# 拒绝 .. / \ null byte
if '..' in filename or '/' in filename or '\\' in filename:
    raise ValueError("Invalid filename")

# 二次防护: 只用 basename
resolved = (session_dir / os.path.basename(filename)).resolve()
if not str(resolved).startswith(str(session_dir.resolve())):
    raise ValueError("Path traversal detected")
```

### 13.4 构建脚本安全

`qt_build` 在生成 `.bat`/`.sh` 脚本前必须：
- 验证所有路径输入不含控制字符（ASCII < 0x20，除 `\t` 外）、不含引号字符 `"`、不含换行符
- 优先方案：使用 `subprocess.Popen` 直接执行 vcvars/qtenv2 并捕获环境变量，避免生成中间脚本文件

### 13.5 工作区互斥

```python
# 启动时在 .qt-commander/server.lock 上获取文件锁
# 使用 fcntl.flock(LOCK_EX | LOCK_NB) 或 msvcrt.locking()
# 若锁已被占用 → 立即退出并报错
```

### 13.6 端口文件清理

- 库：写入时使用 `O_CREAT | O_EXCL` + owner-only 权限（0600 / owner DACL）
- 注入器：读完后立即删除端口文件
- Python MCP：`connect_with_retry` 成功后确认端口文件已删除

## 14. 会话持久化与恢复

### 14.1 会话元数据

每个 session 目录包含 `session.json`：

```json
{"pid": 1234, "port": 45678, "token": "a3f9...", "lib_path": "/path/to/libqt-commander.dll", "created_at": "2026-07-30T10:00:00Z"}
```

`SessionManager.create()` 在注入成功后写入；`destroy()` 在 detach 后删除。

### 14.2 启动时恢复

```python
async def recover_on_startup():
    for session_dir in sessions_path.iterdir():
        meta = json.loads(session_dir / "session.json")
        # 检查 PID 是否仍存活
        if not psutil.pid_exists(meta["pid"]):
            # 清理孤儿会话：删除目录
            shutil.rmtree(session_dir)
            continue
        # 尝试验证注入库仍在运行
        if await test_connection(meta["port"], meta["token"]):
            # 恢复会话
            sessions[meta["id"]] = await Session.reconnect(meta)
        else:
            # 尝试 eject 后清理
            await eject_if_possible(meta["pid"], meta["lib_path"])
            shutil.rmtree(session_dir)
```

## 15. 构建缓存验证

### 15.1 源哈希检查

```
构建时：计算 native/src/ 下所有 .cpp/.h 的 SHA-256 → 写入 .qt-commander/build/build_manifest.json
启动时：重新计算当前源码哈希 → 与 manifest 比较 → 不匹配 → BuildState = NOT_BUILT
```

### 15.2 产物完整性检查

```python
def verify_artifacts():
    required = [
        BUILD_DIR / "injector" / "build" / "qt-injector.exe",  # 或平台后缀
        BUILD_DIR / "library" / "build" / "libqt-commander.dll",
    ]
    return all(p.exists() and p.stat().st_size > 0 for p in required)
```

## 16. RPC 超时与流恢复

### 16.1 超时后重连

当 `send_rpc` 超时（`asyncio.wait_for(30s)`）：
1. 关闭当前 TCP 连接
2. 不必尝试 drain 残留帧——连接可能已失步
3. 使用 `connect_with_retry` 重新建立 TCP 连接
4. 重新认证（`qt.authenticate`）
5. 递增请求 ID 重置计数器
6. 重试原 RPC 调用

### 16.2 注入器超时清理

当 subprocess 超时（60s）：
1. 调用 `proc.kill()`
2. 尝试 `qt-injector --eject <pid> <lib_path>` 作为清理步骤（best-effort）
3. 删除端口文件
4. 返回 `InjectionError(code=2002)`

## 17. TCP Keepalive（修正）

- 旧 spec 声称 "5s 间隔"——不切实际
- 实际配置：`SO_KEEPALIVE` + idle=60s + interval=10s + count=3
- 总检测时间：60 + 10×3 = 90 秒
- 应用层心跳：每 30 秒发送 `{"jsonrpc":"2.0","method":"ping","id":0}` 帧检测僵尸连接

## 18. fastmcp 错误处理（后备方案）

当前设计依赖 `mcp.custom_error_handler`，若 fastmcp >= 2.0 不提供此 API：

```python
# 后备方案：工具内部 try/except 返回 JSON-RPC 格式的 error 字典
def _tool_error(code: int, message: str) -> str:
    return json.dumps({
        "error": {"code": -32000, "message": message, "data": {"code": code}}
    })

@mcp.tool()
async def qt_attach(pid: int) -> str:
    try:
        ...
    except BuildRequiredError:
        return _tool_error(2001, "Build required")
    except SessionExistsError:
        return _tool_error(2006, f"Process {pid} already attached")
```

此方案与 MCP 协议兼容（工具返回字符串即 `text` content），且不依赖 fastmcp 内部 API。

```
旧结构                              新结构
────────                              ────────
src/                                  src/
├── server/        ← 删除             ├── injector/       ← 新（独立 CLI）
│   ├── mcp/                          │   ├── main.cpp
│   ├── session/                      │   └── injector.*
│   ├── inject/    → 移入 injector/   ├── library/        ← 不变
│   ├── process/   ← 删除             │   └── ...
│   └── build/     ← 删除             └── common/         ← 精简
├── library/       ← 不变                 ├── framing.h    ← header-only
├── common/        ← 精简                 └── socket_utils.*
│   ├── framing.cpp → 删除
│   └── framing.h   → header-only

tools/                                 mcp_server/         ← 新
├── quick_inject.cpp  ← 删除           ├── pyproject.toml
                                       ├── server.py
tests/                                 ├── session.py
├── unit_server/    ← pytest           ├── rpc_client.py
├── unit_library/   ← 不变             ├── builder.py
└── integration/    ← pytest           ├── framing.py
                                       └── native/
   新增: unit_injector/                    └── src/...
```
