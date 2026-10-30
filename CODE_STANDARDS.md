# qt-commander 代码规范

> 适用范围：`qt_commander/`（Python MCP server）、`src/`（C++ injector / library）、`tests/`、`scripts/`。
> 目标不是“风格统一”本身，而是：**可复用、可测试、可诊断、可演进**。
> 参考并提炼自：[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/)、[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)、[Qt Coding Style](https://wiki.qt.io/Qt_Coding_Style)、[Qt Coding Conventions](https://wiki.qt.io/Coding_Conventions)、[PEP 8](https://peps.python.org/pep-0008/)、[Ruff](https://docs.astral.sh/ruff/)、[SEI CERT C++](https://wiki.sei.cmu.edu/confluence/pages/viewpage.action?pageId=88046682)。

---

## 0. 三条总原则

1. **先找轮子，再写代码**：Qt / STL / Python 标准库 / 成熟 PyPI 包能做的，不自己写；确需自研的，在 PR/提交里写明「为什么现成的不能用」。
2. **单一事实来源**：协议帧、JSON-RPC、错误码、遍历规则、节点投影、属性序列化、会话状态，各自只能有一份实现。
3. **可测、可诊断**：每个失败都有 code/cause/hint；每个性能声明都有基准；每个公共行为都有测试。

---

## 1. 复用优先（不重复造轮子）

### 必须复用

| 领域 | 用什么 | 不要写 |
|---|---|---|
| JSON | `QJsonDocument` / Python `json` | 手写 JSON 解析/转义 |
| 帧协议 | `src/common/framing.*` / `qt_commander/framing.py` | 第二套 length-prefix |
| TCP/socket | `src/common/socket_utils.*` / `asyncio` | 平台 socket 封装复制 |
| 进程/环境探测 | `psutil`、`QProcessEnvironment`、`vswhere` | 自己遍历 `/proc` 的重复实现（除必要平台补充） |
| 正则 | `QRegularExpression` / Python `re` | 手写匹配器 |
| 日志 | 统一 logger（Python）/ `qCDebug` 分类（C++） | `print`/`printf` 散落 |
| 错误 | 统一错误码表 + 错误对象 | 每个模块自定义字符串 |
| 遍历/投影 | 单一 walker + 单一 projector | query/items/act 各写一套 |
| MCP | `FastMCP` | 自己实现 MCP 传输 |
| CLI/参数 | `argparse` / `QCommandLineParser` | 手写 argv 解析 |

### 新增工具的准入

新增一个 util/helper 前必须回答：

1. Qt/STL/stdlib 里有没有等价物？
2. 仓库里是否已有同职责模块？
3. 放在哪个层？是否会造成反向依赖？
4. 谁测试它？

答不上来就不要新增；先重构现有模块。

### 禁止的“轮子”

- 自研 JSON-RPC / MCP 协议。
- 自研线程池、定时器、future（用 `asyncio` / Qt）。
- 自研日志轮转、配置解析。
- 自研字符串格式化、路径拼接（用 `std::filesystem` / `pathlib`）。
- 为每个控件复制一份遍历/矩形计算；用 provider/adapter 注册表。

---

## 2. C++ / Qt

### 2.1 语言与所有权

- C++17；不使用编译器扩展。
- **RAII**：资源在析构释放；禁止裸 `new`/`delete` 管理所有权。
  - Qt 父子对象：`new` 时必须立即 `setParent` 或交给 layout/容器。
  - 非 QObject：`std::unique_ptr`；共享所有权才用 `std::shared_ptr`。
  - QObject 观察者：`QPointer`，不持有裸指针跨轮次。
- 禁止 owning raw pointer；函数参数用 `T*` 表示可空观察，`T&` 表示非空。
- `const` 正确性：不修改的成员函数、参数、局部变量都加 `const`。
- `enum class`；`nullptr`；`override`/`final`；删除不需要的拷贝/移动。
- 命名空间：库代码放 `qt_commander::`；匿名命名空间用于文件内实现。
- 头文件：`#pragma once`；前向声明优先；不把私有头类型暴露到公共 API。
- 宏只用于条件编译/平台隔离，不用来定义函数或常量。

### 2.2 Qt 约定

- 字符串：`QStringLiteral`、`QLatin1String`；避免在热路径反复构造 `QString`。
- 信号槽：新式 `connect`；需要重载时用 `qOverload`。
- 用户可见文本：`tr()`；内部标识不用 `tr()`。
- JSON：`QJsonObject/QJsonArray/QJsonDocument`；不手拼字符串。
- 文件写入：原子写（temp + rename / `QSaveFile`）；不覆盖半写文件。
- 正则：`QRegularExpression`；热路径预编译。
- 计时：`QElapsedTimer`；不用 `clock()`。
- 锁：`QMutex`/`QReadWriteLock`；不在持锁时发信号/调外部代码。
- 线程：GUI 对象只在 GUI 线程访问；RPC 线程只做 IO/调度；跨线程用队列/信号。
- 私有头（QPA / QML / QStyleOption）：集中到 `src/library/compat/`，按 Qt 版本隔离；公共 API 不暴露私有类型。
- 禁止在 `DllMain` 里做线程、socket、堆分配、Qt 调用。

### 2.3 错误与边界

- 不允许异常穿过 C ABI / DLL 边界；库导出函数返回错误码/结构。
- 错误必须带：`code`（稳定字符串/枚举）、`message`（人读）、`hint`（下一步）、`details`（机器可读）。
- 失败路径必须有测试；不要只测 happy path。
- 输入校验在边界做一次；内部信任已校验数据。
- 资源失败（文件、socket、DLL）必须回滚已获得的资源。

### 2.4 风格与工具

- 4 空格缩进；大括号 Qt 风格（函数/类左括号换行，控制语句同行）。
- 函数尽量短（目标 < 60 行）；文件按职责拆分（目标 < 800 行）；超了要说明理由。
- 命名：类型 `PascalCase`，函数/变量 `camelCase`，成员 `snake_case_` 或 `m_`（全仓库统一一种），常量 `kPascalCase`。
- 注释解释**为什么**，不解释代码在做什么；过时注释视同 bug。
- `clang-format`（基于 Qt 风格）统一格式；`clang-tidy` 检查 bugprone/performance/modernize。
- 编译警告：MSVC `/W4`、GCC/Clang `-Wall -Wextra`；CI 中警告即失败（至少新增警告即失败）。

### 2.5 性能

- 先测量再优化；每个优化提交附基准数据。
- 不在 GUI 线程做全树属性读取、全量 JSON、磁盘 IO。
- 避免 O(n²)：遍历一次、命中才投影；大表只枚举可见项。
- 复用元对象信息（类级属性计划），不重复 `property("text")` 字符串查找。
- 不在热路径分配大对象；必要时复用 buffer/缓存，但要有失效策略。

---

## 3. Python

### 3.1 语言与风格

- Python 3.10+；4 空格；行宽 100（ruff 配置）。
- 类型标注：公共函数、数据结构、工具函数必须标注；内部短函数可省略。
- `pathlib.Path` 代替字符串路径；`dataclass`/`Enum` 表达结构化数据。
- 上下文管理器管理文件/子进程/锁；禁止裸 `open` 不关闭。
- 异常：捕获具体异常；禁止 `except Exception: pass`（清理路径可 `suppress` 但要注释）。
- 日志：模块级 `logger = logging.getLogger(__name__)`；不 `print`（CLI 输出除外）。
- 子进程：显式 `timeout`；参数用列表，`shell=False`；捕获 stderr 用于错误信息。
- 异步：`async def` 内不调用阻塞 IO/`subprocess.run`；必须用 `asyncio.to_thread` 或异步子进程。
- 字符串：f-string；JSON 用 `json.dumps(..., ensure_ascii=False)`。

### 3.2 分层

```
server.py        仅 MCP 注册、参数校验、结果封装
session.py       会话生命周期、RPC 调度、锁
rpc_client.py    injector/协议客户端
builder.py       构建编排
environment_detector.py / process_detector.py  探测
occlusion.py     纯几何后处理
errors.py        错误码/错误对象
```

- 工具函数不写业务逻辑；业务逻辑放独立模块，便于单测。
- 不允许 `server.py` 出现巨大的 if/else 或内联协议解析。
- 公共数据结构用 dataclass/TypedDict；避免裸 dict 在层间传递（协议边界除外）。

### 3.3 工具链

- `ruff check` + `ruff format`；`pytest`；`pytest-asyncio`。
- 新增依赖必须有明确理由；优先标准库。
- 测试：单元测试不依赖真实注入；E2E 单独标记；禁止 `time.sleep` 作为同步手段（用轮询/事件）。

---

## 4. 架构与接口

- **依赖方向**：工具层 → 会话/编排 → 协议客户端 → 库；反向依赖禁止。
- **单一协议编解码**：Python 与 C++ 各一份，但字段/错误码表同源；修改必须同步测试。
- **单一遍历/投影**：query、items、act、screenshot 共用 walker/projector；不允许复制遍历逻辑。
- **单一错误表**：错误码只在 `errors.py` / C++ 错误头定义；其它模块引用，不硬编码字符串。
- **接口设计**：
  - 一个字段一个含义；不用同一个字段承载两种语义。
  - 默认值显式、可预测；不在深层隐式改变行为。
  - 返回结构化数据；错误作为数据返回（`ok:false`），不靠异常传业务错误。
  - 破坏性操作明确标注，并要求显式 opt-in（如 `purge`、`input:"api"`）。
- **状态**：不引入隐藏全局可变状态；会话状态集中在 Session/SessionManager；测试可注入。
- **可观测性**：关键路径有结构化日志/计时；错误带上下文；不记录 token/敏感数据。

---

## 5. 测试

- 测试金字塔：单元（纯函数/组件）→ 集成（协议/会话）→ E2E（真实注入/链接 + 测试应用）。
- 每个 bug 修复先加失败测试；每个公共 API 有正常/边界/错误测试。
- 测试确定性：固定种子/固定布局；不依赖屏幕分辨率、网络、真实时间。
- 不用任意 sleep；用条件轮询 + 超时；超时失败要带诊断信息。
- E2E 测试应用单独构建；测试结束清理进程/会话/临时文件。
- 性能测试单独标记；阈值回归即失败；记录硬件/OS/Qt 版本。
- 覆盖率不是目标，但核心路径（协议、遍历、错误映射、会话生命周期）必须覆盖。

---

## 6. 安全

- 只操作本机、同用户、有权限的进程；不做提权、不绕过 EDR/反作弊。
- token 不写日志；descriptor/session 文件 0600；目录 0700；原子写；拒绝 symlink。
- 所有外部输入（JSON、路径、正则、HTML/文本）都校验；路径做穿越检查。
- 子进程参数不拼 shell；路径加引号；超时；失败回滚。
- 不把目标应用的属性/文本直接当代码/命令执行。
- JS 断言只读上下文；不暴露裸 QObject；超时/中断；禁止 `eval`/`new Function`。

---

## 7. 文档

- 公共 API（工具、RPC 方法、C++ 导出）必须有 docstring/注释：参数、返回、错误、副作用。
- 错误码表、协议字段表、节点 schema 保持单一来源；文档与代码同步。
- 设计决策记录在 `dev-docs/`；实现偏离设计时更新文档或写明理由。
- 注释不重复代码；写约束、坑、为什么。

---

## 8. 多轮代码评审（子代理）

### 评审分组（每轮至少 2 个视角）

| 视角 | 关注 |
|---|---|
| 正确性/安全 | 边界、错误路径、并发、资源泄漏、权限、注入、路径穿越 |
| 设计与复用 | 分层、重复实现、接口一致性、命名、可测试性、是否造轮子 |
| 性能/资源 | GUI 线程、O(n²)、分配、超时、内存、缓存失效 |
| 测试/文档 | 覆盖、确定性、失败诊断、文档/错误码同步 |

### 流程

1. 实现者提交变更说明 + 自测结果 + 已知风险。
2. 评审子代理只读代码/测试，输出：**阻塞项**（必须修）、**建议项**（可选）、**证据**（文件:行/测试）。
3. 实现者修复阻塞项；对建议项逐条回复（采纳/拒绝 + 理由）。
4. 第二轮评审验证修复；仍有阻塞项则继续，直到清零。
5. 评审记录归档到 `dev-docs/reviews/`（不进入发布物）。

### 评审红线

- 重复实现已有模块（帧/JSON/遍历/错误/日志）→ 阻塞。
- 无测试的错误路径/边界 → 阻塞。
- 裸 QObject 暴露给 JS、token 泄漏、路径穿越 → 阻塞。
- 未测量的性能“优化”或明显 GUI 线程阻塞 → 阻塞。
- 只改风格、不改行为的意见 → 非阻塞（交给 formatter）。

---

## 9. Definition of Done

一个变更完成必须满足：

- [ ] 构建通过（Qt5/Qt6 相关目标；Python 导入通过）
- [ ] 单测/集成测试通过；新增行为有测试
- [ ] 无新增编译警告；ruff/clang-format 干净
- [ ] 错误路径有明确 code/hint；日志不含敏感信息
- [ ] 没有重复实现；复用了现有模块
- [ ] 性能敏感路径有基准或预算说明
- [ ] 文档/错误码/协议表同步
- [ ] 至少一轮多视角子代理评审，阻塞项清零
