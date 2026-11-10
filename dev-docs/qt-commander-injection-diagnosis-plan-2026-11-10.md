# 注入失败诊断与改进方案（对 `mcp-injection-workflow-feedback-2026-11-10.md` 的核实）

> 状态：**仅分析，未改代码**。等待你通知后再实施。
> 核实环境：本机真实目标 `RealEvo-Qt5SylixOS-QtCreator.exe`（pid 3680），2026-11-10 14:2x。
> 方法：读源码 + 在 `.venv` 里直接调用 `qt_commander.target/artifacts/server` 的现有函数做只读验证。

## 1. 结论先说

报告描述的**信息缺口真实存在**，应做；但报告对**根因的判断是错的**。

真实根因是：`qt_target_info` 把 **Release** 目标误报成 `debug: true`，于是 artifact 匹配选中了 **Debug** 版注入库；Debug 库链接 `Qt5Cored.dll`，在目标进程里加载**第二份 Qt**，那份 Qt 的 `QCoreApplication::instance()` 永远是空，于是注入库等满 10s 后写下 `timeout waiting for QCoreApplication`。报告把它解释为“旧的 `qt_commander_inject.dll` 已加载，需要重启”，并据此设计了 P0（报告 loaded_injector / restart_required）。

也就是说：

- 报告要的“可恢复性信息”仍然值得做（错误确实不可诊断）；
- 但如果只按报告做，**下次 attach 依然会挑错 artifact**，问题照旧复现；
- 必须先修 `debug`/构建风味判定，否则 `qt_build(for_pid=...)` 这类“自动推导”功能会把错误放大（自动推导出 Debug 然后自动构建错 artifact）。

## 2. 现场证据（可复现）

### 2.1 目标其实是 Release

```text
exe        D:\Develop\RealEVO-Qt5\qtcreator-mainline\build\Desktop_Qt_5_15_2_MSVC2019_64bit_Release\bin\RealEvo-Qt5SylixOS-QtCreator.exe
已加载 Qt   C:\Software\Qt\5.15.2\msvc2019_64\bin\Qt5Core.dll      ← 无 d 后缀
已加载注入库 D:\Develop\qt-commander\artifacts\windows-x64\msvc-19.44\qt-5.15.2-release\lib\qt_commander_inject.dll
descriptor  ...\descriptor-3680.json  library_build_id = "qt5/MSVC-AMD64/Release"
```

### 2.2 但 `qt_target_info` 说 debug=true

```text
target_info(3680).debug              = True      ← 错
target_info(3680).evidence           = [Qt5Core.dll, Qt5Cored.dll]   ← 同一目录里两套都在
target_info(3680).compiler_version   = "19.44.35228"  （来自 descriptor）
```

原因在 `qt_commander/target.py:300 _scan_runtime_dirs()`：它按目录遍历，**后出现的那个 Qt5Core*d*.dll 覆盖前一个**，`debug` 就是“最后一个匹配项的 d 后缀”。

`_path_runtime()`（`target.py:357`）扫描目标进程 `PATH` 的前 64 项，落到 Qt kit 的 `bin` 目录：

```text
C:\Software\Qt\5.15.2\msvc2019_64\bin -> ['Qt5Core.dll', 'Qt5Cored.pdb', 'Qt5Cored.dll', ...]
```

一个完整 Qt 安装的 `bin` 里 release/debug 两套 DLL 都在，所以 `debug` 取决于目录枚举顺序 —— 既是错的，也是**不确定的**。

### 2.3 因此一个干净的 attach 现在仍然会失败

刚测的（目标正处于正常工作状态）：

```text
target_info(3680).debug = True
artifacts.find() -> artifacts\windows-x64\msvc-19.44\qt-5.15.2-debug   ← build_type=Debug
```

也就是说：如果现在重启目标、不带 `artifact_dir` 直接 `qt_attach(pid=..., mode="injection")`，会**再次**注入 Debug 库、**再次**拿到 `timeout waiting for QCoreApplication`。这个 bug 今天是活的。

### 2.4 时间线（artifacts / build / 进程）

```text
14:20:44  build\windows-x64_msvc-19.44_qt-5.15.2-debug\x86_64-release?  实际是 debug 构建
14:20:44  artifacts\...\msvc-19.44\qt-5.15.2-debug\lib\qt_commander_inject.dll   ← 报告里那次重建
14:23:39  artifacts\...\qt-5.15.2-debug\bin\qtc-inject.exe
14:23:53  build\windows-x64_msvc-19.44_qt-5.15.2-release
14:24:33  artifacts\...\msvc-19.44\qt-5.15.2-release\{bin,lib,manifest}   ← 后来补的 Release 构建
14:24:39  目标进程启动
14:24:58  descriptor-3680.json（library_build_id .../Release）
```

即：Debug 构建（报告里的那一次）确实没用；真正让它跑起来的是随后一次 **Release** 构建 + 重启。报告没有记录这一步，也没有意识到“Debug 这个选择本身是 `qt_target_info` 带出来的”。

### 2.5 另一条独立缺陷：Linux 侧没有 status 回传

`src/library/inject_main.cpp:58-95`（Windows）会把失败原因写 `%TEMP%\qtc-inject-status-<pid>.txt`；`inject_main_unix.cpp` 完全没有。所以 Linux 上同类失败只能落到 `E_TARGET_NOT_QT`（“DLL loaded but no descriptor appeared”）——这条误导性错误在 Linux 上是 100% 覆盖。本次不一定要动 C++，但方案里要标注清楚。

## 3. 逐条核实报告

| 报告项 | 核实 | 说明 |
| --- | --- | --- |
| `qt_attach` 失败只给 `E_INJECTION_FAILED` + 原始 message | **成立** | `server.py:293` 直接 `_error(INJECTION_FAILED, status文本)`，无 hint、无 details、`retryable=false` |
| 没有任何工具报告“已加载的 injector DLL” | **成立** | 全仓库 Python 侧搜 `qt_commander_inject` 只有构建拷贝逻辑；`_scan_loaded_modules` 只找 Qt Core / CRT |
| 没有报告“来自哪个 artifact / build id” | **成立** | 同上；`manifest.json` 里也没有 `build_id` 字段（报告说的 build_id 是 `qt_build` 返回值现算的） |
| 没有报告“需要重启目标” | **成立** | `restart_required` 不存在；README“Known limitations”里只有文字说明 |
| `qt_build` 只能死给 4 个必填参数 | **成立** | `source_dir` 是必填位置参数（`server.py:741`），无 `for_pid`；重建参数只能靠读 README/CMakeLists |
| `qt_environments` 只有 Qt kit 和 toolchain | **成立** | `server.py:446` 只返回 `qt` / `toolchains` |
| 工具描述缺关键约定 | **部分成立** | `qt_build` 描述里已有“build type 必须匹配”；但 artifact 根/命名、重启要求、`source_dir` 示例都没有 |
| 根因 = “旧 DLL 已加载，需要重启” | **不成立（这是误判）** | 见 §2。真实根因是 debug 风味误判 → 注入 Debug 库。重启之所以“有效”，是因为重启顺带换成了 Release artifact |
| “已加载再注入”是安全无提示的 | **成立，另一条** | `inject.py:105` 走 `LoadLibraryW`，同路径已加载时只是引用计数 +1、`DllMain` 不再调用 → 无 descriptor、无 status → 落 `E_TARGET_NOT_QT`（`server.py:293`）。误导比报告描述的更严重，只是这次事件不是它 |

另外一处**文档与实现不符**（报告没提，但和它的 P0 “artifact_mismatch” 直接相关）：

- `qt_attach` 的 `build_id` 参数在描述和 `INSTRUCTIONS` 里写着“guards against … a mismatch must be rebuilt/reinjected”，实现是 `server.py:1038` → `E_NOT_IMPLEMENTED`。今天**没有任何 build/架构不匹配校验**，`INSTRUCTIONS` 里“If attach rejects a build/architecture mismatch…”是一条不存在的路径。
- build_id 还有两套格式：descriptor/编译期 `qt5/MSVC-AMD64/Release`（`src/CMakeLists.txt:21`），`qt_build` 返回 `qt5.15.2/msvc-x64/Debug`（`server.py:871`）。agent 拿两者比对会误判成“不匹配”。

## 4. 方案

原则：**Python-only**（不改 C++、不重建 artifact，避免动你正在跑的注入会话）；先修根因，再加可见性，最后补 `for_pid`。

### P0-1 修 debug/风味判定（根因）

落点 `qt_commander/target.py`：

1. `target_info()` 的证据优先级改为：**目标进程已加载模块** → 可执行文件同目录 → 加载器 `PATH` → descriptor。
   已经现成存在 `_module_runtime()`（`target.py:440` → `_scan_loaded_modules()` `target.py:393`），对 pid 3680 实测 **17 ms** 且答案正确：
   ```text
   {'qt_major': 5, 'qt_version': '5.15.2', 'debug': False, 'toolchain': 'msvc',
    'evidence': ['C:\\Software\\Qt\\5.15.2\\msvc2019_64\\bin\\Qt5Core.dll', ...]}
   ```
   它读的是“真的被加载的那个 Qt”，`debug` 由 `Qt5Core*d*.dll` 直接判定，不存在二义。
2. `_scan_runtime_dirs()`：一个目录里同时出现 `Qt5Core.dll` 和 `Qt5Cored.dll` 时，**不再瞎猜**——版本可以报，`debug` 置 `None` 并在 `evidence` 里记 `ambiguous(<dir>)`。这样 PATH 兜底路径永远不会再撒谎。
3. `qt_version` 同理：从加载模块的 PE version 取（已有 `_file_version`），不要靠“扫目录最后命中”。
4. 结果里加一个 `detected_from: "module"|"adjacent"|"loader_path"|"descriptor"`，让调用方自己判断可信度（`confidence` 现在是按 exe 能否解析算的，和 Qt 风味无关，不够用）。

配套 `qt_commander/artifacts.py:136 _matches()`：`target.debug is None` 时不要再当 `False` 处理（`bool(None)`），而是当“未知”，走 `find()` 已有的歧义路径返回 `E_ARTIFACT_AMBIGUOUS` + candidates + `hint`。宁可不选，也不静默选错。

> 不做的替代方案：让 `debug` 未知时默认挑 release。会把“悄悄做错”保留下来，只是换了个方向，不采纳。

### P0-2 注入状态可见 + 失败可诊断

新增 `target.loaded_injector(pid)`（同文件，`psutil.memory_maps`，实测 Windows/Linux 都可用）：

```text
loaded_injector = {
  loaded: bool,
  path: "…/lib/qt_commander_inject.dll",      # 进程里真实那个
  artifact_dir: "…/qt-5.15.2-release",        # 由 path 反推
  artifact: {qt_version, toolchain, compiler_version, qt_debug, build_type},  # 读同目录 manifest.json
  build_id: "qt5.15.2/msvc-x64/Release",      # 规范化后的 id（见 P1-1）
  library_mtime: "…",
  stale: bool                                 # 见下
}
```

`stale` 判定（不读内存、只看文件与进程时间）：`library 文件 mtime > 目标进程 create_time`，或 `artifact_dir != 当前匹配到的 artifact_dir`。

`target_info()` 增加：

```text
loaded_injector,                                    # 上面那个
expected_artifact: ".../qt-5.15.2-release",         # artifacts.find 会选谁
injection_state: "none" | "loaded" | "stale" | "mismatch",
restart_required: bool
```

`restart_required` 规则：有 `qt_commander_inject` 模块已加载，且（无 live descriptor ∨ `stale` ∨ `artifact_dir != expected`）。

`qt_attach` 失败改成结构化错误（沿用现有 envelope，不新造 schema）：

```json
{"ok": false, "error": {
  "code": "E_INJECTION_FAILED",
  "reason": "qt_runtime_flavor_mismatch",
  "message": "timeout waiting for QCoreApplication",
  "retryable": false,
  "restart_required": true,
  "hint": "…一句可执行的话…",
  "details": {
    "target": {"pid": 3680, "qt_version": "5.15.2", "debug": false, "toolchain": "msvc", "arch": "x64"},
    "artifact": {"directory": "…/qt-5.15.2-debug", "build_id": "qt5.15.2/msvc-x64/Debug", "qt_debug": true},
    "loaded_injector": {"loaded": false},
    "expected_artifact": "…/qt-5.15.2-release"
  }}}
```

`reason` 取值（报告建议的那组，去掉重叠项）：

| reason | 触发 | hint 要点 |
| --- | --- | --- |
| `injector_already_loaded` | 注入前扫到同路径模块已加载 | 重启目标；不要重复 LoadLibrary |
| `artifact_missing` | `artifacts.find` → `E_ARTIFACT_NOT_FOUND` | 调 `qt_build(for_pid=...)` |
| `artifact_ambiguous` | 同上 `E_ARTIFACT_AMBIGUOUS` | 传 `artifact_dir=<candidates>` |
| `qt_runtime_flavor_mismatch` | status=`timeout waiting for QCoreApplication` 且 artifact 的 debug ≠ 目标已加载 Qt 的 debug | 明确写“Debug 库会加载 Qt5Cored.dll，目标是 Release” |
| `timeout_waiting_qcoreapp` | 同上但无法判定风味 | 报告目标已加载的 Qt / 库的 debug |
| `target_not_ready` | DLL 已加载、无 status、无 descriptor，且无已加载注入模块 | 可能目标还没到 QApplication |
| `permission_denied` | `E_PERMISSION` | 提权 / ptrace 权限 |
| `target_not_qt` | descriptor 超时且注入前无 Qt 证据 | — |

另外两点行为改动（都很小，收益大）：

1. 注入**前**先扫 `loaded_injector`：已加载就直接回 `injector_already_loaded` + `restart_required`，不再走一次无意义的 `LoadLibraryW`。
2. `_await_descriptor_for_pid()`（`server.py:236`）现在固定等 20s 才去读 status，而 DLL 10s 就写完了。改为边等边 poll status 文件，~10.5s 出结果，且拿到的是精确原因。

### P0-3 `qt_build` 支持 `for_pid` 并允许省略 `source_dir`

`qt_build` 签名改为：

```python
async def qt_build(
    source_dir: str | None = None,
    build_dir: str | None = None,
    qt_major: int | None = None,      # for_pid 时从目标推导
    toolchain: str | None = None,
    arch: str | None = None,
    build_type: str | None = None,
    target: str | None = None,
    qt_env: str | None = None,
    vcvars_path: str | None = None,
    extra_args: list[str] | None = None,
    timeout_s: int = 300,
    wait_s: float = 20.0,
    for_pid: int | None = None,
) -> dict[str, Any]:
```

- `source_dir` 省略时：默认 `artifacts.ROOT.parent`（在开发 checkout 里就是 `D:\Develop\qt-commander`），并**校验 `CMakeLists.txt` 存在**；安装在 site-packages 里时回落到明确报错 + 提示传 `source_dir`。不要复用 `QT_COMMANDER_SOURCE_DIR`（那个变量已经被 `qt_source` 用作“目标的源码树”，语义冲突）。
- `for_pid` 生效时按 `target_info(pid)` 推导：`qt_major`、`toolchain`、`arch`、`build_type`（`debug` → Debug/Release）、Qt kit（版本 + 编译器家族双重过滤）、`vcvars_path`。
  **注意本机有 VS2022(14.44) 和 VS2026(14.51) 两套 MSVC**：必须优先选“abbrev 等于 `target_info.compiler_version`”的那套（本例 19.44），否则 artifact 名会变成 `msvc-19.51`，和 descriptor 里的 19.44 对不上 → `E_ARTIFACT_NOT_FOUND` 死循环。
- 显式传参优先于推导（比如强制 `build_type=RelWithDebInfo`）。
- 推导不出来时**不要降级成默认值**，直接报错并列出需要补的参数（`debug` 未知就是典型）。
- 返回值补 `restart_required` + `next_step`（“重启目标进程后 qt_attach(pid=…)”）；构建成功后顺手跑一次 `artifacts.find(target)`，回 `matches_target: true/false`，把闭环做掉。

### P1-1 统一 build_id 与 manifest

- `manifest.json` 增加 `build_id`，用和 `qt_build` 返回值同一个格式（`qt5.15.2/msvc-x64/Release`），由 `write_manifest()` 从已有字段拼出来即可（schema 1 → 2，`read_manifest` 不校验 schema，向后兼容）。
- descriptor 里的编译期 id 格式不同（C++ 侧 `src/CMakeLists.txt:21`）。P1 先做“解析 + 双字段输出”（`artifact_build_id` / `library_build_id`），**不动 C++**；将来要统一再动宏。
- `Session.build_id` 目前永远是 `None`（`session.py:25/126`，没人赋值），`qt_status` 里那个字段是死的——顺手填上或删掉，别留假字段。

### P1-2 `qt_environments` 补自助信息

```json
{"qt": [...], "toolchains": [...],
 "qt_commander": {
   "source_dir": "D:\\Develop\\qt-commander",
   "artifact_root": "D:\\Develop\\qt-commander\\artifacts",
   "artifacts": [{"directory": "...", "build_id": "...", "build_type": "Release"}],
   "suggested_build_call": {"tool": "qt_build", "arguments": {"for_pid": 3680}}
 }}
```

`suggested_build_call` 只在“能唯一确定目标”（显式 pid，或恰好一个 live descriptor）时给；否则省略，别猜。

### P2 文档与工具描述

- `README.md`：Injection artifacts 段补 `build_id` 与 manifest 字段；Known limitations 里那条“重建+重启”补上 **debug/release 必须与目标进程实际加载的 Qt 一致**（并说明 `qt_target_info.detected_from` 怎么读）。
- `docs/design.md` §4 / §7.1：补 `qt_build(for_pid=...)`、`loaded_injector`、`injection_state`、`reason` 枚举、build_id 规范；§7.4 auto 决策树补“已加载未服务”分支。
- `server.py` 的 `INSTRUCTIONS`：把“attach 会拒绝 build/架构不匹配”改成事实（当前无此校验），并在操作环第 2 步写“先看 `qt_target_info.loaded_injector` / `restart_required`”。
- 三个工具的 docstring 补：artifact 根目录与命名规则、`source_dir` 默认值、注入失败即需重启、Debug/Release 必须匹配。
- `qt_attach(build_id=...)`：**建议这一步只改文档**（标注 reserved），不实现硬校验——两套 build_id 格式统一之前，硬校验有把正常会话拒掉的风险。是否实现留给你定（见 §7 决策点）。

## 5. 改动清单

| 文件 | 改动 | 风险 |
| --- | --- | --- |
| `qt_commander/target.py` | 证据优先级、双 Qt 目录不猜、`loaded_injector()`、`injection_state`/`restart_required`/`detected_from` | 中（`target_info`/`qt_processes` 表现变化） |
| `qt_commander/artifacts.py` | `debug is None` 不当 False；`write_manifest` 写 `build_id` | 低 |
| `qt_commander/inject.py` | 注入前已加载短路；把底层失败映射成 reason | 低 |
| `qt_commander/server.py` | `_inject_target` 结构化错误、status 轮询、`qt_build(for_pid)`、`qt_environments`/`qt_status` 补充、INSTRUCTIONS/docstring | 中（`qt_build` 签名） |
| `tests/unit_server/*` | 见 §6 | — |
| `README.md` / `docs/design.md` / `dev-docs/` | 文档 | 无 |
| C++ | **不动**（Linux status 回传列为可选后续） | — |

## 6. 测试计划

新增/调整（全部纯 Python，可离线跑）：

1. `test_target.py`
   - 双 Qt 目录：`Qt5Core.dll` + `Qt5Cored.dll` 同目录 → `debug is None` + evidence 标注。
   - 模块扫描优先：monkeypatch `memory_maps` 返回 release `Qt5Core.dll`，同时 PATH 侧给 debug 目录 → `target_info` 报 `debug=False`、`detected_from="module"`。（现有 autouse fixture 会关模块扫描，需显式开）
   - `loaded_injector`：monkeypatch 模块列表 → `loaded/artifact_dir/build_id`；`library mtime > create_time` → `stale=True`。
   - `injection_state`/`restart_required` 四态各一例。
2. `test_artifacts.py`
   - `target.debug=None` + 两套 flavor 都在 → `E_ARTIFACT_AMBIGUOUS` + candidates（不静默选 release）。
   - `write_manifest` 产出 `build_id`，且格式与 `qt_build` 返回一致。
3. `test_server.py`
   - `qt_attach` 失败：已加载 → `reason=injector_already_loaded` + `restart_required`；status 文件 `timeout waiting for QCoreApplication` + artifact Debug/目标 Release → `reason=qt_runtime_flavor_mismatch`（不新增 20s 等待，monkeypatch 时间）。
   - `qt_build(source_dir=None)` → 默认仓库根；`for_pid` 推导（monkeypatch `target_info`/`detect_qt_environments`/`detect_toolchains`）→ 选中 19.44 的 vcvars、artifact 目录 `msvc-19.44`、`restart_required=true`。
   - `for_pid` 且 `debug` 未知 → 报错并列出需补参数（不落默认值）。
4. `test_prompts.py`
   - 保持 21 个工具不变（**不新增 `qt_artifacts` 工具**，`qt_status` 已有 artifacts 列表，状态归 `qt_target_info`）；新增断言：INSTRUCTIONS 提到 `restart_required`、`loaded_injector`、`for_pid`；`qt_attach` 描述不再声称有 build_id 校验。
5. 端到端（需要你同意后、用**测试 app** 而非你的 Qt Creator）
   - Release 测试 app：`qt_build(for_pid=...)` → `qt_attach` 成功。
   - 反向回归：手工把 Debug artifact 塞给同一个 Release app → 必须在 ~10s 内返回 `qt_runtime_flavor_mismatch`，且 hint 指出 Qt5Cored 问题。
   - 已加载路径：注入成功→手动删 descriptor→再 attach → `injector_already_loaded` + `restart_required`，进程内只应有一个注入库模块。

## 7. 需要你拍板的点

1. **`qt_attach(build_id=...)`**：只改文档（推荐，零风险）／实现软校验（返回 `build_mismatch` 警告但继续）／实现硬拒绝。
2. **`debug` 未知时**：返回 `E_ARTIFACT_AMBIGUOUS`（推荐，不静默猜）／默认挑 release（可用性优先，但可能再次注入错库）。
3. **收尾口径**：`for_pid` 是否同时接受“无 pid、只有唯一 live descriptor”的自动推导（推荐是）。
4. 是否顺手补 `restart_required` 对应的**自动重启**能力：我认为不要做（越权，且 `INSTRUCTIONS` 明确“进程启动/退出不在工具范围”）。

## 8. 交付顺序（每步都可独立验证）

1. P0-1（`target.py` + `artifacts.py`）—— 这一步就能让今天的失败复现路径恢复正常；用 pid 3680 只读验证 `debug=False`。
2. P0-2（`loaded_injector` + 结构化错误 + status 轮询）。
3. P0-3（`qt_build(for_pid)` + 默认 `source_dir`）。
4. P1（manifest build_id、environments 自助块、session.build_id）。
5. P2（README / design / INSTRUCTIONS / docstring）。
6. 全量 `uv run pytest tests/unit_server -q`；本次不重建 artifact，故**不需要重启你的目标进程**，只需在你方便时重启 MCP server。

## 9. 附：本次核实用的只读命令（可复核）

```powershell
# 进程里真实加载的模块
.venv\Scripts\python.exe -c "import psutil;[print(m.path) for m in psutil.Process(3680).memory_maps() if 'qt' in m.path.lower() and 'core' in m.path.lower()]"
# 工具当前输出 vs 实际
.venv\Scripts\python.exe -c "from qt_commander import target;print(target.target_info(pid=3680)['debug'])"
.venv\Scripts\python.exe -c "from qt_commander import target,artifacts;print(artifacts.find(target.target_info(pid=3680))['directory'])"
```

---

# 实施记录（2026-11-10，已完成）

已按本文 §4 的方案实施（Python-only，未改 C++、未重建 artifact）。

## 决策（采用方案里的推荐项）

1. `qt_attach(build_id=...)`：**只改文档**，保留 `E_NOT_IMPLEMENTED`，hint 指向 `loaded_injector.build_id` / `expected_artifact.build_id`。避免在两套 build_id 格式统一前硬校验误伤正常会话。
2. `debug` 未知时：`E_ARTIFACT_AMBIGUOUS` + candidates，**不静默选 release**。
3. `for_pid`：只按显式 pid 推导；不额外做“无 pid + 唯一 live descriptor”的隐式推导（`qt_environments.suggested_build_call` 已在无副作用的位置给出建议）。
4. 不做自动重启目标。

## 改动文件

| 文件 | 内容 |
| --- | --- |
| `qt_commander/target.py` | `_scan_runtime_dirs` 双 Qt 目录不再猜（`debug=None` + `flavor_ambiguous`）；`target_info` 证据优先级改为 module → adjacent → loader_path → descriptor，新增 `detected_from`、`expected_artifact(_error)`、`loaded_injector`、`injection_state`、`restart_required`；新增 `_module_paths`/`loaded_injector`/`injection_state` |
| `qt_commander/artifacts.py` | `compose_build_id`/`manifest_build_id`；manifest schema 2 写 `build_id`；`_matches` 不再把 `debug=None` 当 release；歧义候选带 `build_id/build_type`；`listing()` 带 `build_id` |
| `qt_commander/server.py` | `_inject_target` 结构化失败（`reason`/`restart_required`/`details`）+ 注入前已加载短路 + status 轮询（20s→约 10.5s）+ `target_gone`；`qt_build` 支持 `for_pid` 与可省略 `source_dir`，MSVC toolset 按目标编译器版本挑选，返回 `matches_target/restart_required/next_step`；`qt_environments` 增加 `qt_commander` 块与 `suggested_build_call`；`Session.build_id` 填值；INSTRUCTIONS 与各工具描述同步 |
| `tests/unit_server/*` | 新增 17 个用例（风味判定、已加载检测、stale、状态机、attach 三种失败原因、for_pid 推导、未知风味拒绝、source_dir 默认、build_id 规范化、prompt 契约） |
| `README.md` / `docs/design.md` | 注入 artifact/manifest/build_id、loader 证据规则、失败原因表、重启语义、build_id 参数状态 |

## 验证

```text
.venv\Scripts\python.exe -m pytest tests/unit_server -q   → 123 passed
```

真实目标（只读）：

```text
pid 26880 RealEvo-Qt5SylixOS-QtCreator（Release 构建，注入着 release 库）
  target_info: debug=false, detected_from=module, injection_state=loaded,
               restart_required=false, expected=...qt-5.15.2-release
  （改动前：debug=true，expected=...qt-5.15.2-debug）
pid 45400 RealEvo-Qt5SylixOS-QtCreator（Debug 构建）
  target_info: debug=true, detected_from=module, expected=...qt-5.15.2-debug
  → 两个不同风味的目标现在各自判对；旧逻辑对两者都报 debug=true
```

端到端（用 `tests/test-apps/injected` 自建 app，`.test-workspace/injection_e2e_check.py`，不碰 Qt Creator）：

```text
1 target_info          debug=true, detected_from=module, expected=.../Debug
2 用 Release artifact 注入 Debug 目标
                       → reason=qt_runtime_flavor_mismatch, restart_required,
                         hint 指出 Qt5Core.dll vs debug 目标, 13.0s（原 20s + 无信息）
3 用匹配 artifact       → ok, session 建立
4 删掉 descriptor 再 attach
                       → reason=injector_already_loaded, restart_required,
                         loaded_injector{build_id, artifact, matches_expected}
                         且没有发生第二次 LoadLibrary
```

## 上线方式

- 纯 Python 改动：**只需重启 MCP server**；目标进程、artifact 都不用动。
- 未做（后续可选）：Linux `inject_main_unix.cpp` 的 status 回传（现在 Linux 上这类失败仍只能落到 `target_not_ready`）；descriptor 与 manifest 两套 build_id 格式合一；`qt_attach` 的 build-id 硬校验。
