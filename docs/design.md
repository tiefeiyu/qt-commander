# qt-commander 设计基线

> 状态：设计定稿（2026-10-29）。范围：完整重写，不保留旧实现。
> 待补：`qt_items` 覆盖率与 provider 链（子代理）、`qt_source` 降级契约（子代理）、P0 数值校准。

---

## 1. 目标与原则

1. **Live-first**：UI 是活的；不拍快照、不建索引副本，`query` / `items` / `act` 都在目标进程里实时求值。
2. **Raw object tree**：对象树只做忠实记录——几何、身份、子节点、按需属性；不做 role/type/text/label/interactive 之类的语义投影。
3. **Locator-first**：Agent 用「类/属性/文本/QML id/上下文」寻址，字符串句柄只作回执；动作在库内解析，避免 TOCTOU。
4. **Budgeted projection**：任何返回都有节点/属性/图像预算，默认只给够用的一屏；超出返回明确错误，不静默截断。
5. **Action = transaction**：一次 `qt_act` 完成解析、执行、settle、观察、expect；动作自己回报是否达成预期。
6. **Typed failure**：失败是结构化数据（code / cause / retryable / hint / details），不是一句文案。
7. **两种接入模式**：injection（闭源/黑盒）与 linked（白盒/有源码）；同一套协议与工具。

---

## 2. 架构

```
Agent
  │ stdio, 16 tools
  ▼
Python Orchestrator
  ├─ RPC client：多请求在途、绝对死线、幂等重试、错误归一
  ├─ Act engine：expect 轮询、settle、候选消歧
  ├─ Source index（可选，白盒）
  └─ Build / Session / Rendezvous manager
  │ JSON-RPC（控制请求走 RPC 线程，数据请求进 GUI 线程；图像走二进制帧）
  ▼
In-process Engine (C++/Qt)
  ├─ Walker：单遍遍历，继承状态，类级属性计划
  ├─ Locator evaluator：便宜谓词先过滤，命中才投影
  ├─ ElementRegistry：字符串句柄，单调、永不重用、带会话标签
  ├─ ActionExecutor：真实 QPA 管线，复合动作，op 账本
  ├─ Virtual items providers：adapter → QAccessible → extension（待定稿）
  ├─ Probe：RPC 线程直接应答，GUI 卡死也能 ping
  └─ Screenshot：同步抓取、缩放、标注、编码，二进制回传
```

---

## 3. 数据模型

### 3.1 节点

```json
{
  "id": "7f3a.42",
  "class": "QPushButton",
  "qml_type": null,
  "objectName": "saveBtn",
  "qml_id": "",
  "rect": {"x": 10, "y": 20, "w": 80, "h": 28},
  "child_count": 0,
  "children": [],
  "props": {},
  "matched": true,
  "screen_pos": {"x": 100, "y": 200},
  "items": {"kind": "table", "total": 2000, "visible": 200}
}
```

- 核心字段永远存在，用空值表达缺失：`objectName:""`、`qml_id:""`、`qml_type:null`、`props:{}`、`children:[]`、`child_count:0`。
- `matched` 仅在带 `where` 的响应里出现；一旦出现，每个节点都有（命中 `true`，上下文 `false`）。
- `screen_pos` 仅根节点；`items` 仅支持非 QObject 子项的宿主（可选字段）。
- `qml_type` 来自 Qt 的 `QQmlMetaType::prettyTypeName`（失败回落类名去 `_QMLTYPE_\d+` / `_QML_\d+`）；非 QML 为 `null`。
- `child_count` 是活树中的**可视子节点数**（遍历规则固定），不等于 `children.length` 时表示有未展开子节点。
- `virtual` / `kind` 只出现在 `qt_items` 的节点里，不混进 `children`。

### 3.2 坐标

`qt_query` 增加响应级参数：

```python
coords: "parent" | "screen"   # 默认 "parent"
```

- `parent`：每个 `rect` 相对父节点内容区；根节点固定 `{0,0,w,h}`，屏幕位置放 `screen_pos`。
- `screen`：每个 `rect` 都是桌面坐标；根节点 `rect` 即其桌面矩形；`screen_pos` 冗余但保留。
- 一个响应里所有节点同一空间；不在节点上带 `space`。
- `screen` 模式下无法映射的节点：`rect:null` + `unmapped:true` + `rect_error`；`parent` 模式永远有值。
- Wayland：`coords:"screen"` 返回 `E_COORDS_UNAVAILABLE`，提示用 `parent`/window；不静默给 0 坐标。

几何是 raw：不乘 DPR、不处理 transform。QML 旋转/缩放在 `screen` 模式下取轴对齐包围盒并标 `transformed:true`。

### 3.3 属性

`props` 是谓词过滤器：

```json
{
  "name": ".*",
  "name_regex": false,
  "type": "QString",
  "type_regex": false,
  "flags": ["readable"],
  "exclude": ["styleSheet", "palette"],
  "dynamic": true,
  "max": 30,
  "meta": false
}
```

- `name_regex` / `type_regex` 默认 `false`；`false` 为字面量精确，`true` 为锚定全匹配 PCRE。
- `exclude` 跟随 `name_regex`；应用顺序：name/type/flags 选中 → exclude → max。
- `flags`：`readable`（隐含）、`writable`、`designable`、`user`、`stored`、`constant`、`final`、`required`、`bindable`、`enum`、`flag`、`notify`。
- `max` 超出时截断并标 `props_truncated:true`、`props_total:N`。
- `dynamic` 控制 `dynamicPropertyNames`；动态属性 `meta.dynamic:true`，类型取运行时值。
- `meta:true` 返回 `props_meta`（类型、flags、enum 名、dynamic）。

`props` 的形态：

```python
props: null            # 不读属性（默认）
props: []              # 不读
props: "*"             # 全部（受 max）
props: ["text","checked"]   # 精确名简写
props: { ...过滤器... }
```

`where.props` 是匹配条件，只接受精确属性名，不支持过滤器：

```json
{"where": {"props": {"text": "Save", "enabled": false}}}
{"where": {"props": {"text": {"contains": "Sav"}}}}
```

操作符：裸值 = eq；`ne`、`contains`、`gt`、`gte`、`lt`、`lte`；多个操作符 AND。命中的属性自动回显到结果的 `props`。

值序列化：

| 类型 | 返回 |
|---|---|
| bool / int / double / QString | JSON 原生 |
| enum / flag | int；`meta:true` 附 enum 名 |
| QRect / QSize / QPoint / QColor / QFont / QUrl | 字符串 |
| QVariantList / QVariantMap | 递归 JSON，限深度/元素数 |
| QObject* / QPixmap / QImage / QIcon | 跳过 |
| unreadable / write-only | 跳过；`meta:true` 可列元信息 |

---

## 4. 工具面（15）

| 工具 | 签名与要点 |
|---|---|
| `qt_status` | `(session_id=None)`；构建、进程、内存会话、磁盘可 resume 会话、平台能力；有 session 时带 live ping |
| `qt_environments` | 发现 VS / MinGW / Qt |
| `qt_build` | `(toolchain, qt_major, arch, build_type, qt_env, vcvars_path?, ...)`；单一 build（QML+JS） |
| `qt_attach` | `(pid=None, session_id=None, build_id=None, mode="auto")`；pid 新接入，session_id 手动 resume |
| `qt_detach` | `(session_id, purge=False)` |
| `qt_query` | `(where?, scope?, props=None, coords="parent", max_depth=-1, limit=200, include_hidden=False, occlusion=False, count_only=False)` |
| `qt_items` | `(owner, where?, limit=50, offset=0, include_hidden=False)`；非 QObject 子项 |
| `qt_act` | 单一动作入口；见 §5.3 |
| `qt_batch` | `(steps, stop_on_error=True)`；steps 是 act/wait 的数组 |
| `qt_wait` | `(condition, timeout_ms=2000)`；条件同 expect |
| `qt_screenshot` | `(target?, max_width=1024, annotate="none", where=None, max_labels=50, format="png")` |
| `qt_get` | `(target, props=None)`；单节点/虚拟节点读取 |
| `qt_set` | `(target, name, value)`；destructive |
| `qt_call` | `(target, method, args=None)`；destructive；不返回 QObject* |
| `qt_source` | `(target, context_lines=0)`；源码位置/handler，待子代理定稿 |

---

## 5. 查询、items、动作、expect、截图

### 5.1 `qt_query`

- `where`：locator，字段 `class`、`inherits`、`object_name`、`qml_id`、`props`、`within`、`window`；全部 AND。
- `scope`：`{id}` 或 `{window}`；缺省所有顶层窗口。
- 输出嵌套树；根是顶层窗口（或 scope 节点）。
- `where` 存在时返回剪枝树：命中节点带 `matched:true`，祖先保留结构。
- `count_only:true` 只返回 `{count}`，不建树、不序列化；expect 轮询用它。
- **all-or-nothing**：结果超过 `limit` 或硬上限（2000 节点 / 2 MB）→ `E_RESULT_TOO_LARGE`，不返回部分树。
- `include_hidden`：默认剪掉隐藏子树；`true` 才纳入。
- `occlusion:true`：对返回节点计算 `visible_ratio`/`covered_by`，只在同窗口内计算；窗口之间永不互相遮挡。
- 顺序确定：窗口顺序 + 树序；不随机。

### 5.2 `qt_items`

> 当前建议：独立工具 + provider 链；覆盖率结论回来后定稿。

- provider 顺序：adapter（菜单/表格/tab/表头/slider/ComboBox）→ QAccessible（其他标准子控件/可访问自绘控件）→ extension（白盒测试钩子）。
- 每个节点带 `source:"adapter"|"accessible"|"extension"`。
- 虚拟节点 schema：

```json
{
  "id": "7f3a.vc.42.3.5",
  "virtual": true,
  "kind": "cell",
  "class": "QAbstractItemView::cell",
  "parent": "7f3a.42",
  "rect": {"x": 0, "y": 72, "w": 180, "h": 24},
  "props": {"text": "foo", "row": 3, "column": 5, "selected": false, "enabled": true},
  "child_count": 0,
  "children": []
}
```

- `kind`：`menu_item`、`cell`、`tab`、`header_section`、`slider_handle`、`accessible`。
- 虚拟 id 自描述：`<tag>.v<kind>.<owner>.<index>`；宿主销毁即失效。
- 默认只返回可见项；`total`/`visible`/`truncated` 支持分页。
- 没有 provider 能覆盖时：`E_NO_ITEMS`，提示截图 + 元素相对坐标。

### 5.3 `qt_act`

```python
qt_act(
    target,                  # locator 或 {id}
    action,                  # click|dblclick|hover|type|key|focus|scroll|drag
    nth=0,
    position=None,           # 元素相对坐标；缺省中心
    button="left",
    modifiers=None,
    text=None, clear=False, submit=False,   # type
    keys=None,               # key
    dx=0.0, dy=0.0,          # scroll
    to=None,                 # drag: locator 或元素相对点
    duration_ms=300,         # drag
    expect=None,
    timeout_ms=2000,
    settle_ms=0,
    observe=True,
    input="auto",            # auto|pointer|api（invoke，待定）
    js_risk_ack=False,
)
```

- 单一工具；`select`/`check` 不单列，用 click 组合。
- 动作在库内、GUI 线程、执行前解析 locator；strict：多个命中返回 `E_AMBIGUOUS` + 候选，不猜。
- 默认真实 QPA 管线；`input:"api"` 只对虚拟节点的 `invoke` 生效（待定）。
- 返回：

```json
{
  "ok": true,
  "action": "click",
  "resolved": {"id": "...", "class": "...", "props": {...}},
  "dispatched": true,
  "settled_ms": 52,
  "observed": {"id": "...", "props": {...}},
  "expect": {"matched": true, "after_ms": 120, "node": {...}},
  "replayed": false
}
```

- `op_id` 由 Python 内部生成，不暴露给 Agent；2004/2003 时同 `op_id` 自动重试一次；库缓存 120s。
- expect 超时：`ok:false` + `cause:"expect_not_met"` + `dispatched:true` + `retryable:false`，附 `observed`。

### 5.4 expect

```json
{"expect": {"props": {"enabled": false}}}
{"expect": {"where": {"class": "QDialog"}, "gone": true}}
{"expect": [{"where": {"id": "s7f3.42"}, "props": {"enabled": false}},
            {"js": "$ui.count({class:'QDialog'}) === 0"}]}
```

- 两种形态 **严格互斥**：
  - 声明式：`{where?, props?, gone?}`；`where` 缺省主体为 resolved 元素；`props` 并入 `where.props`；`gone` 与 `props` 互斥。
  - JS：`{js}`；`$target` = resolved 元素快照（可为 null），`$ui` = 只读 `count/find/get/prop`。
- 数组 = AND；每个 item 仍是两种形态之一。
- 语法错误在 dispatch 前校验；动作不执行。
- JS 约束：
  - 只读上下文、`Object.freeze`、strict mode；绝不暴露目标 QObject。
  - 单表达式；硬禁 `eval`/`new Function`/`async`/`await`/`delete`/`function`。
  - 静态扫描 `for`/`while`/`do` → `E_JS_RISK`，Agent 带 `js_risk_ack:true` 重试。
  - watchdog 20ms（纯 JS）；`$ui` helper 50ms deadline、短小非阻塞。
  - 返回必须布尔；非布尔 `E_JS_NOT_BOOLEAN`。
- 轮询：50–100ms，`count_only` 轻查询；超时 2000ms 默认。

### 5.5 `qt_screenshot`

- `max_width`：缺省 1024；`0` = 原始设备像素；`>0` 只降不升；`<0` 参数错误；超 64MB 帧 → `E_IMAGE_TOO_LARGE`。
- `annotate`：`none`（默认）、`interactive`、`matched`（配合 `where`）、`all`；编号 + legend；对象实线、虚拟虚线；`max_labels=50`。
- 一致性：一次 GUI 线程任务内「枚举 → 同步抓图 → 画框 → 返回」；QML 用 `grabWindow` 裁剪，不用异步 `grabToImage`。
- 返回 image + `legend`（`n/id/class/text/virtual/rect/image_rect`）+ `image{width,height,scale,dpr,bytes,format}`。
- 原生对话框/全屏桌面截图不在范围内；Wayland 下窗口/元素截图可用。

### 5.6 `qt_source`

> 子代理已核实 Qt 5.15.2 / 6.8.3 私有头与运行时行为；默认「位置可用、源码可缺」。

```python
qt_source(target, handler=None, snippet_lines=3, max_lines=20)
```

```json
{
  "available": true,
  "code": null,
  "kind": "qml",
  "target": {"id": "7f3a.42", "class": "...", "objectName": "..."},
  "location": {
    "url": "qrc:/qml/Main.qml",
    "file": ".../qml/Main.qml",
    "line": 42,
    "column": 8,
    "origin": "qrc",
    "confidence": "high",
    "stale": false
  },
  "qml": {"type": "FactTextField", "objectName": "", "handler": "onClicked"},
  "snippet": {"startLine": 39, "lines": ["..."]},
  "diagnostics": []
}
```

- QML 位置：`QQmlData::get(obj)->lineNumber/columnNumber` + `compilationUnit->url()`；Qt5 嵌套对象 `compilationUnit` 可能为空，回退 `outerContext->url()`。
- qrc：Qt6 默认保留 `.qml` 源码（`QFile(":/...")` 可读）；`QT_DISCARD_FILE_CONTENTS` 会把内容置空（size=0），此时 `available:false, code:"E_SOURCE_DISCARDED"`。
- 映射顺序：原路径存在 → `path_map` 最长前缀 → `qrc_map` → 相对路径 → basename 唯一；多义 `E_MAPPING_AMBIGUOUS`，不猜。
- Widgets：`.ui` 不在运行时通常不可得；PDB/DWARF 只到类构造/方法行；`objectName`+`.ui`（qrc/本地）才能精确到 `.ui` 行。
- 版本校验：`unitData()->qtVersion` vs `qVersion()`；manifest/source hash；不符 `stale:true, confidence:"low"`。
- 默认不扫全盘、不下载 PDB；`source_root`/`path_map`/`qrc_map`/`ui_roots`/`manifest` 显式配置；snippet 默认 3 行、上限 20 行/8KB；root 外不返回本地路径。
- Qt5/6 分别编译（`QQmlData` 布局不同）；启动 `qVersion()` 门控；未知版本 `E_UNSUPPORTED_QT`。
- 错误：`E_NO_LOCATION`、`E_SOURCE_UNAVAILABLE`、`E_SOURCE_DISCARDED`、`E_SOURCE_STALE`、`E_NO_DEBUGINFO`、`E_MAPPING_AMBIGUOUS`、`E_HANDLER_NOT_FOUND`、`E_UNSUPPORTED_QT`。
- `available` 仅指源码文本；位置本身永远尽量返回，不因源码缺失报 RPC 错误。

---

## 6. 协议

### 6.1 帧

```
[u32 magic = 0x51544332][u32 json_len][u32 bin_len][json][bin]
```

- UTF-8 JSON；`bin` 专放图像；单帧上限 64 MiB。
- 不再有 `jsonrpc` 字段；极简信封。

### 6.2 握手

```json
{"id":1,"method":"hello","params":{"protocol":2,"token":"...","client":"...","resume":false}}
```

响应：

```json
{"id":1,"result":{
  "protocol": 2,
  "library_version": "2.0.0",
  "capabilities": ["qml", "expect_js", "virtual_items", "binary_frames"],
  "session_tag": "7f3a",
  "limits": {"max_frame": 67108864, "max_inflight": 8}
}}
```

- 失败：`E_AUTH_FAILED` / `E_PROTOCOL`，关闭连接。

### 6.3 请求与并发

```json
{"id":42,"method":"query","params":{...},"timeout_ms":5000,"op_id":"..."}
```

- 方法：`ping`、`cancel`、`shutdown`、`query`、`act`、`get`、`set`、`call`、`screenshot`、`eval`、`items`、`source`。
- 响应：`{id,result}` / `{id,error}`。
- RPC 线程非阻塞：投递 GUI 后继续读；响应按 id 匹配、可乱序。
- `ping`/`cancel` 在 RPC 线程直接应答；GUI 卡死也能回。
- 在途上限 8；超出 `E_BUSY`；`ping` 报 `inflight`/`queued`。

### 6.4 死线与幂等

| 情况 | 错误 | 执行过 | 重试 |
|---|---|---|---|
| 出队/执行前过期 | `E_EXPIRED` | 确定没有 | 安全 |
| GUI 执行超时 | `E_TARGET_TIMEOUT` | 可能 | 同 `op_id` |
| 队列满/重入 | `E_BUSY` | 确定没有 | 安全 |
| 取消（排队中） | `E_CANCELLED` | 确定没有 | 安全 |

- `timeout_ms` 随请求发送；库出队/执行前各查一次。
- `op_id` 仅 `act`/`set`/`call`；缓存 120s；重复返回 `replayed:true`。

### 6.5 字符串 id

- 对象：`<tag>.<counter>`，如 `7f3a.42`。
- 虚拟：`<tag>.v<kind>.<owner>.<index>`，如 `7f3a.vc.42.3.5`。
- 单调、永不重用；跨会话不误中；Agent 只当不透明字符串。

---

## 7. 会话、构建、linked mode

### 7.1 构建

单一 build：QML + JS 永远开启。

```
build_id = f"{qt_version}/{toolchain}-{arch}/{build_type}"
build_dir = workspace/builds/build_id
```

例：

```
builds/qt5.15.2/msvc2019-x64/release/
builds/qt6.8.3/mingw1310-x64/release/
```

manifest：

```json
{
  "build_id": "qt5.15.2/msvc2019-x64/release",
  "qt_version": "5.15.2",
  "qt_major": 5,
  "toolchain": "msvc",
  "toolchain_version": "msvc2019",
  "arch": "x64",
  "build_type": "release",
  "source_hash": "...",
  "built_at": "...",
  "paths": {"injector": "...", "library": "..."}
}
```

- 同一路径 rebuild in place；source_hash 一致复用。
- `qt_build` 不再接受 `with_qml`/`with_js`。
- `qt_attach` 匹配四个维度；不做 Quick 选 build；`build_id` 显式覆盖。

### 7.2 会话与恢复

- 一 PID 一会话；不自动 resume。
- 启动只扫描磁盘会话（不连接）；`qt_status` 列出 `pid/pid_alive/build_id/grace_remaining_ms`。
- 手动 `qt_attach(session_id=...)` resume；宽限 60s；过期 `E_SESSION_EXPIRED`。
- 库在客户端断开后保留 listener/ElementMap/op 缓存，宽限内允许重连；`shutdown` 立即释放。

### 7.3 linked mode

白盒专属：应用链接库并调用 `qt_commander::start()`。

```cpp
#include <qt_commander/agent.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qt_commander::StartOptions opts;
    opts.name = "myapp";
    if (auto r = qt_commander::start(opts); !r)
        qWarning() << "qt-commander:" << r.error;
    // ...
}
```

- `start()` 必须在 GUI 线程、`QApplication` 之后；失败不致命。
- 库绑定 127.0.0.1:0、生成 token、启动 RPC、原子写 descriptor。
- rendezvous：TCP，不用 UDS 主路径（macOS `sun_path` 104B、Windows Python asyncio 无 `AF_UNIX`）。
- 路径：Linux `$XDG_RUNTIME_DIR`（回退 `$TMPDIR` 0700）；macOS `Application Support`/`$TMPDIR`；Windows `%LOCALAPPDATA%`；`QT_COMMANDER_ENDPOINT` 覆盖。
- descriptor schema 2：

```json
{
  "schema": 2,
  "protocol": {"major": 2, "minor": 0},
  "session_id": "...",
  "mode": "linked",
  "pid": 1234,
  "process_start_time": 1690000000.123,
  "started_at": "...",
  "exe_path": "...",
  "nonce": "...",
  "port": 45678,
  "token": "...",
  "library_version": "2.0.0",
  "library_build_id": "qt5.15.2/msvc2019-x64/release",
  "qt": {"major": 5, "version": "5.15.2"},
  "capabilities": ["qml", "expect_js", "virtual_items"],
  "scope": {"uid": 1000, "pid_ns": "...", "boot_id": "..."}
}
```

- `process_start_time` 由库读 OS 真实创建时间；服务端与 `psutil.create_time()` 等值比对（≤0.1s）。
- 目录 0700 + 属主校验；文件 0600；`O_NOFOLLOW`/`O_EXCL`；临时文件 + rename；Windows DACL。
- hello 回显 `pid/session_id/mode/protocol/process_start_time/nonce`；token 常量时间比较；accept 循环，认证失败只关连接。

### 7.4 auto 决策树

```
pid 不存在 → E_PROCESS_GONE
live = descriptor 中 start_time≈create_time 且 exe_path/pid_ns 相同者
|live| > 1 → E_AMBIGUOUS_SESSION
|live| = 1 →
    socket 属主≠pid → E_LINKED_BUSY
    connect + hello：
      identity 不符 → E_LINKED_IDENTITY
      token 拒 → E_AUTH_FAILED
      连不上且进程在 → E_LINKED_UNREACHABLE
      连不上且进程死 → GC → inject
ds 非空且 live 空 → GC → inject
畸形 descriptor → E_LINKED_INVALID（不 inject）
显式 mode=linked 且无 live → E_LINKED_NOT_FOUND
inject：
    库答已有会话 → E_SESSION_EXISTS
    受保护/反注入 → E_INJECTION_BLOCKED
    权限不足 → E_PERMISSION
    非 Qt → E_TARGET_NOT_QT
    其它 → E_INJECTION_FAILED
```

原则：

- 有 live descriptor：只能 connect，不能 inject。
- 无 descriptor：只能 inject，不能“顺手试试 linked”。
- 不跨模式兜底；auto 永不抛 `E_LINKED_NOT_FOUND`。
- 显式 `mode=injection` 且存在 live descriptor → `E_MODE_CONFLICT`。
- `build_id` 在 linked 下忽略 + warning。
- 闭源应用没有 descriptor，只走 inject；inject 失败不兜底 linked。

---

## 8. 平台支持

| 平台 | 闭源（仅注入） | 白盒 linked | 条件 |
|---|---|---|---|
| Windows x64 | 可用 | 可用 | CIG/PPL/EDR 拦截则不可行；同架构/CRT；建议 delay-load |
| Linux X11 | 可用（native ptrace） | 可用 | 需要 Yama 允许：父进程注入或 `ptrace_scope=0` |
| Linux Wayland | 可用（native ptrace） | 可用 | linked 不依赖 ptrace；XCB 输入/截图受 Wayland 限制 |
| macOS arm64/x64 | 不可行（无注入器） | 可用 | Hardened Runtime 需 disable-library-validation 或同 Team 签名 |

- 官方支持：白盒 linked 三平台；闭源注入 Windows（MinGW/MSVC）与 Linux（native ptrace）。
- macOS 闭源明确不支持；错误 hint 说明用 linked。
- Qt 版本必须与库一致（私有头）；不跨版本。
- Wayland：`coords:"screen"` 报 `E_COORDS_UNAVAILABLE`；不静默给 0。

---

## 9. 性能 P0 与测试应用

### 9.1 测试应用

| App | 规模 | 覆盖 |
|---|---|---|
| `widget-basic` | ~40 | 菜单/工具栏/表单/tab/对话框/右键菜单/禁用隐藏 |
| `widget-complex` | ~1k 可见 / 2k 总 | 嵌套布局、splitter、dock、MDI、隐藏 tab、200×10 表格、tree/list、表头、动态增删 |
| `widget-large` | 7k+ | 生成深树/宽树、500×20 表格、1000 隐藏控件、5 窗口 |
| `qml-basic` | ~40 | Controls、自定义组件、Menu/Popup/Dialog |
| `qml-complex` | 1k–7k | StackView、ListView 1000 项、GridView/TreeView、Canvas、transform、clip、动画 |
| `mixed-qquickwidget` | ~100+QML | QQuickWidget 离屏窗口/host 映射 |
| `custom-painted` | ~20 | 自绘控件，两变体：QAccessible / 无 |
| `multi-window-dpi` | ~200 | 多窗口、负坐标、最小化/最大化、`QT_SCALE_FACTOR` |
| `dynamic-async` | ~100 | 动画、延迟 toast、异步加载 |
| `occlusion` | ~100 | z-order/opacity/clip/部分覆盖/popup |

公共 harness：`ScenarioApp`、`--nodes`/`--scenario`/`--ready-file`/`--auto-close`；稳定 objectName/qml id；禁止原生对话框；CMake target；CI smoke + bench。

### 9.2 指标与阈值（P0 校准）

| 指标 | 目标（7k 节点） |
|---|---|
| query class/objectName P95 | < 10 ms |
| query text P95 | < 35 ms |
| query interactive P95 | < 50 ms |
| 默认 payload | < 32 KB |
| items（200 可见单元格）P95 | < 20 ms，< 32 KB |
| act dispatch P95 | < 15 ms |
| expect count_only 轮询 P95 | < 10 ms |
| JS 纯表达式 | < 1 ms |
| screenshot 1024 P95 | < 150 ms |
| screenshot original 4K P95 | < 500 ms |
| annotate 额外 | < 20 ms |
| GUI 阻塞 | 常规 < 50 ms；full/原图 < 100/500 ms |
| 典型闭环 | ≤ 3 calls、≤ 16 KB、0 图 |
| 图像 | 1024 PNG < 3 MB；4K < 20 MB |

**基准原则**：P0 只测量当前实现的 `query`/`items`/`act`/`screenshot`，不保留旧实现的快照基线。

**当前实测（7000 节点，MinGW5 Debug，`tests/test-apps/large`）**

| 操作 | p50 | p95 | 输出 |
|---|---|---|---|
| query object_name | 26.3 ms | 28.3 ms | 1.6 KB |
| query text | 30.7 ms | 32.9 ms | 2.6 KB |
| query count_only | 5.6 ms | 7.1 ms | 15 B |
| items（表格可见 cell，limit=200，compact） | 15.9 ms | 18.4 ms | 29.6 KB |
| screenshot 1024（默认 active window） | 86.6 ms | 88.2 ms | 1.56 MB |

结论：

- query / count_only / items 的延迟已进入目标区间。
- `items` 200 个 cell 的 payload 在 `compact=true` 下是 29.6 KB，已低于 32 KB；默认 `limit=50` 时约 7.4 KB。benchmark 显式用了 200 + compact。
- 默认窗口截图本次为 86.6 ms（P95 88.2 ms），低于 150 ms；显式 `target` 仍走 `QWidget::grab` 以保证遮挡正确。
- `query object_name` 的早期异常值来自首次布局/字体/渲染冷缓存；benchmark 已加 warmup。

**矩阵 smoke（`tests/test-apps/matrix`，2026-10-29）**

- 覆盖 `basic`、`complex`、`custom-painted`、`multi-window`（含独立窗口/父 dialog）、`multi-mainwindow`、`modal`、`popup`、`occlusion`、`qml`、`mixed`。
- 每个模式启动真实进程，按 objectName 查询，再截 1024 图。
- 实测 query 1.4–2.7 ms，screenshot 49–77 ms，全部模式 count=1，PNG < 1 MB。
- 结果：`docs/benchmarks/matrix.json`。

- 库内 bench 开关（`QT_COMMANDER_BENCH=1`）记录遍历/属性/投影/序列化/抓图分段；生产不付费。
- CI 用参考机 1.5× 回归线；记录 CPU/OS/Qt。
- 覆盖审计：可交互元素总数 vs 对象树/items/QAccessible/视觉兜底。

---

## 10. 错误码

| 组 | 码 |
|---|---|
| 会话 | `E_PROCESS_GONE`、`E_SESSION_EXISTS`、`E_SESSION_EXPIRED`、`E_AMBIGUOUS_SESSION`、`E_MODE_CONFLICT` |
| linked | `E_LINKED_NOT_FOUND`、`E_LINKED_UNREACHABLE`、`E_LINKED_STALE`、`E_LINKED_INVALID`、`E_LINKED_IDENTITY`、`E_LINKED_BUSY`、`E_LINKED_NOT_LOADED` |
| 注入 | `E_INJECTION_BLOCKED`、`E_INJECTION_FAILED`、`E_PERMISSION`、`E_TARGET_NOT_QT` |
| 策略 | `E_POLICY_BLOCKED`（details.policy：hardened/cig/ppl/edr/ptrace_scope） |
| 协议 | `E_PROTOCOL`（major 致命）、`E_AUTH_FAILED`、`E_BUSY`、`E_EXPIRED`、`E_CANCELLED`、`E_TARGET_TIMEOUT` |
| 查询 | `E_NOT_FOUND`、`E_AMBIGUOUS`、`E_HIDDEN`、`E_DISABLED`、`E_STALE_ID`、`E_INVALID_FILTER`、`E_RESULT_TOO_LARGE`、`E_COORDS_UNAVAILABLE` |
| items | `E_NO_ITEMS`、`E_ITEMS_TRUNCATED` |
| expect/JS | `E_EXPECT_TIMEOUT`、`E_JS_SYNTAX`、`E_JS_RUNTIME`、`E_JS_TIMEOUT`、`E_JS_NOT_BOOLEAN`、`E_JS_RISK`、`E_INVALID_EXPECT` |
| 其他 | `E_RENDEZVOUS_UNSAFE`、`E_NAMESPACE`、`E_BUILD_REQUIRED` |

---

## 11. 待定与下一步

1. **`qt_items` provider 链**：覆盖率子代理结论回来后定 adapter/QAccessible/extension 的边界与第一批控件。
2. **`qt_source` 契约**：源码位置/qrc/路径映射/降级；子代理结论回来后定。
3. **linked mode macOS**：补 macOS 构建/CI 验证；Hardened Runtime entitlement 文档。
4. **`qt_act` 的 `input:"api"` / `invoke`**：虚拟节点是否暴露结构化 invoke，待覆盖率结论。
5. **P0 校准**：用测试矩阵跑当前实现，固定阈值。
6. **实现分期**：协议/引擎 → 工具面 → linked → P0 → 文档与 skill。

---

## 12. 附：`qt_items` provider 调研结论（2026-10-29）

两路调研（Spix/同类工具 + Qt 官方机制）结论一致：**对象树必须配 adapter，QAccessible 只做语义补充与兜底，视觉只做最后手段。**

| 控件/子项 | 首选机制 | 覆盖 | 关键坑 |
|---|---|---|---|
| QMenu/QMenuBar 菜单项 | `actions()` + `actionGeometry()`/`actionAt()`；QAction 本身可进 ElementMap | ● | 未 show 时 geometry 空；分隔符/隐藏项；macOS 原生菜单栏无几何 |
| 表格/列表/树单元格 | `visualRect`/`indexAt` + model roles/selectionModel；树 `indexBelow` + `isExpanded` | ● | QAccessibleTable childCount=全模型，不能逐 cell 建接口；代理模型要 mapToSource |
| QComboBox 弹层项 | `view()` + model + `visualRect`；关闭时合成 rect | ● | view 懒建；关闭时 rect 为合成值，act 走 `setCurrentIndex` |
| QTabBar tab | `tabRect`/`tabAt` | ● | tab 可滚出可视区；childCount 含滚动按钮 |
| QHeaderView section | `sectionViewportPosition` + `sectionSize` + `isSectionHidden` | ● | 无 `sectionRect`；负偏移要裁 |
| QSlider handle | `QStyle::subControlRect(CC_Slider, SC_SliderHandle)` | ◐ | `QStyleOptionSlider` 无 `initFrom`，须手填 orientation/min/max/upsideDown |
| QScrollBar 子控件 | `QStyle` subControlRect | ◐ | macOS 覆盖式滚动条无箭头 |
| QSpinBox 按钮/QGroupBox 勾选 | `QStyle` SC_SpinBoxUp/Down、SC_GroupBoxCheckBox | ● | 手填 buttonSymbols/stepEnabled/features |
| 自绘 QWidget | QAccessible（若实现）→ extension → 视觉 | ○/◐ | 默认只有一个 Client 节点，无内部区域 |
| QML | QObject 树 + `Accessible.*` 语义补充 | ◐ | 手写 Item 无 Accessible；Canvas 无子节点 |

工程落点：

- 虚拟元素表：`{ owner QPointer, providerKind, key }`；id 高位打 tag；epoch 重建时一起失效；key 用 `QPersistentModelIndex`（不是 `QModelIndex`）。
- QAccessible：只在 GUI 线程用；不跨轮次持有 `QAccessibleInterface*`；用它的 role/name/rect/action 做**发现**，再用 owner 原生命中测试（`actionAt`/`tabAt`/`indexAt`/`logicalIndexAt`）把 rect 换成可操作句柄。
- QStyle：`subControlRect` 与 Qt hit-test 同源，可信；坐标 widget 局部；空 rect = 该样式无此子控件。
- QPA 输入：`QWindowSystemInterface` 是 QTest 最终入口；窗口局部坐标；不移动真实光标；原生菜单/原生子窗口无效；未显示/最小化/被遮挡会丢事件。
- Spix 的教训：纯对象树 + postEvent 是 adapter 层的最小实现，菜单/单元格/tab/表头/slider handle 一律不可达；选择器/严格模式/可跳级 DFS 值得借鉴，但**不能把“只靠对象树”当覆盖策略**。

`qt_items` 的 `source` 字段取值：`adapter` / `accessible` / `extension`；视觉兜底不产生节点，只返回 `E_NO_ITEMS` + 截图提示。

---

## 13. 开发自测记录

### linked E2E

- `src` 静态库：帧协议 + `QTcpServer` + QWidget 查询/动作/截图/items；`tests/test-apps/linked` 链接后调用 `qt_commander::start()`。
- Qt5 MinGW5 构建：`build/linked-mingw5/qt-linked-test.exe`。
- `scripts/linked_smoke.py`：`hello` → query → `props` → `count_only` → click → type → screenshot；`btnA.text == "Alpha"`，`nameEdit.text` 从 `initial` 变为 `hello`，`statusLabel.text == "clicked"`。
- 截图 480×320 PNG，二进制帧回传；`read_image` 人工确认状态栏和输入框内容正确。
- 查询按节点 schema 返回 `id/class/objectName/qml_type/qml_id/rect/child_count/children/props/matched`，根节点带 `screen_pos`。

### MCP 16 工具

- `qt_commander/server.py` 注册 16 个工具：`status/environments/build/attach/detach/windows/query/items/act/batch/wait/screenshot/get/set/call/source`。
- `scripts/mcp_linked_smoke.py` 通过 stdio 实测：`qt_attach` → `qt_windows`/`qt_query` → `qt_act` → `qt_screenshot` → `qt_detach`；另测 `qt_get`/`qt_set`/`qt_call` 和 `qt_items`。
- 当前可用：linked/injection attach/detach、windows（top-level id/title/active/modal/screen_pos）、query、items、act（click/dblclick/type/focus/key/scroll/hover/drag）、batch/wait、screenshot、get/set/call、source、build、environments。
- `qt_windows` 返回每个 top-level 窗口的 `id/class/objectName/title/rect/screen_pos/screen/visible/active/modal/popup/tooltip/child_count`；父窗口下的 dialog 也会在列表中出现。没有 target 的 `qt_screenshot` 在多个可见 top-level 窗口时返回 `E_AMBIGUOUS`，不会静默选 activeWindow。
- `qt_environments` 扫描显式根/常见 Qt 安装路径/PATH，返回版本、kit、compiler 和可见 C++ 工具链。
- `qt_source` 先按 objectName 搜索 `.ui`；QQuickWidget 再取 `rootObject()->objectName()` 搜索 `.qml`。搜索根为 `QT_COMMANDER_SOURCE_DIR`、可执行目录、当前目录（各最多 500 个文件）。命中返回 `origin:"ui"|"qml"` + file/line/confidence medium；未命中返回 identity + `E_NO_DEBUGINFO`。实测 `.ui` btnA 与 QML quick 均命中。
- `qt_items` 已覆盖 QTabWidget/QTabBar、QMenuBar/QMenu、QHeaderView、QTableView、QTreeView、QListView、QComboBox、QScrollBar、QSlider；combo 关闭时返回合成 rect，`qt_act` 直接 `setCurrentIndex`；虚拟节点带 `virtual/kind/class/parent/rect/props/source`。
- `props:"*"` 枚举 metaObject 可读属性 + dynamic properties，最多 64 个；超出返回 `props_truncated/props_total`。E2E 按钮返回 62 个属性。
- `qt_call` 支持最多 4 个带类型参数（`QMetaMethod` 解析），实测 `setText(QString)`。
- `qt_act` 支持声明式 `expect`（`where/props/gone`，可数组）；Python 轮询 `count_only`/`get`，超时返回 `cause:"expect_not_met"`、`dispatched:true`。E2E：点击 `btnA` 后 `statusLabel.text=="clicked"` 在 1ms 内匹配。
- 严格定位：多候选 `E_AMBIGUOUS`、`nth`、未知键 `E_INVALID_FILTER`、stale id `E_STALE_ID`。
- query 增加 2000 节点 / 2MiB 硬上限；count_only 不受 limit 限制。
- descriptor 改为 `descriptor-<pid>.json`，POSIX 下目录 0700/文件 0600；`aboutToQuit` 清理；Python 扫描并拒绝 symlink/超大/权限过宽。
- descriptor 带 `process_start_time`/`scope`/`library_build_id`；Python 用 `psutil` 比对进程创建时间（0.25s 容差），不一致 → `E_LINKED_STALE`。
- `qt_status(session_id)` 支持 live ping；`qt_attach` 增加 `session_id/mode/build_id`；`qt_detach(purge)` 对未实现 purge 显式报错。
- 虚拟 item id 现在可被 `qt_get` 读取、`qt_act` click/dblclick；tab 用 `QTabWidget` page、menu 用 `QAction`、header 用 logical index、cell 用 `QPersistentModelIndex` 重算 rect，index 仅作 fallback。
- `qt_wait` 用剩余 deadline 作为每次 query 的超时；query 超时返回 `matched:false`。
- `qt_batch` 先校验全部 step 再执行，传输异常保留已完成 steps。
- `qt_build` 已可用：选择发现的 Qt kit，`cmake` configure + build，返回 `build_id/build_dir/output`；MinGW 自动定位编译器；MSVC 通过 `vswhere` 自动发现 VS2022/2026 的 `vcvars64.bat`，也接受显式 `vcvars_path`。实测 Qt5.15.2 msvc2019_64 构建 linked app 成功，并用该二进制度过 MCP attach/query/virtual get/act E2E。
- screenshot 的 `max_width` 按逻辑像素计算，返回 `dpr/pixel_width/pixel_height`；`annotate` 支持 none/interactive/matched/all，返回 `legend`（n/id/objectName/class/text/enabled/focusable/depth/parent_id/virtual/rect/image_rect）。编号直接画在框内 badge，图像不增加右侧 gutter；`n` 是本次截图内的临时编号，agent 用 legend 里的 `id`/`rect` 操作，`n` 仅用于看图。按视觉位置排序，all 只标叶子控件；密码类 QLineEdit 的 text 记为 `***`；返回 annotate_total/shown/truncated；`format` 仅 png。
- 虚拟 item 可 `qt_screenshot(target=virtual_id)`：按注册 rect 裁剪 owner；返回 `virtual:true/target`。
- `qt_status` 的 `descriptor_info` 列出 `pid_alive/session_id/started_at/build_id`。
- Session 记录 `disconnected_at`，`qt_status`/`SessionInfo` 返回 `grace_remaining_ms`；断开后有 60s 重连窗口。
- Native injection 已可用：C++ `qtc-inject`（Windows `CreateRemoteThread(LoadLibraryW)` / Linux `ptrace`+`dlopen`）加载 `qt_commander_inject.dll/.so`；库等待目标 `QCoreApplication` 后在 GUI 线程调用同一 `start()`。Python `inject.py` 只调用 native 可执行，不承载注入机制。MinGW Qt5/Qt6、MSVC Qt5 与 Linux Qt5 注入 app 的 attach/query/act E2E 均通过，脚本 `scripts/injection_smoke.py`。

### 测试

- `uv run pytest tests/unit_server -q`：72 passed。
- linked + MCP stdio E2E：通过（含歧义、nth、stale id、typed call、per-PID descriptor）。

### 待办

1. slider 虚拟 key 仍以 index 为主；annotate 密集标签仍可能重叠。
2. 自定义 model/复杂代理 provider。
3. QML 构建目标与 QML source location。
4. 每个里程碑后跑多视角子代理评审（正确性/安全、设计/复用、性能/测试）。










