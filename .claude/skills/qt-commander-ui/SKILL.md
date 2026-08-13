---
name: qt-commander-ui
description: 驱动 Qt 应用 UI 完成验证闭环（attach/snapshot/find/click/键盘输入/截图）。
  当任务需要操作真实 Qt 应用界面（点按钮、输入文本、验证 UI 状态、截图佐证）时使用；
  排除纯后端/日志验证（用项目自身的 verify 类流程）与进程管理（用项目自身的 run/kill 类流程）。
allowed-tools: mcp__qt-commander__*
---

# qt-commander-ui

驱动已运行 Qt 应用的 UI 完成验证闭环。所有操作通过 qt-commander MCP 工具完成（用户级配置，任何项目可用）。

## 适用场景

- 需要点击按钮、输入文本、验证 UI 状态、截图佐证的验证任务
- 不适用：纯后端/日志验证、进程启停管理（分别用项目自带的 verify/run/kill 类 skill）

## 前置流程

1. **构建库**（首次或源码变更后）：
   - `qt_detect_msvc_and_qt` 检测环境 → 与用户确认构建参数（**不自动选**）
   - `qt_build`（参数须与目标进程匹配，见 references/build-params.md）
2. **启动目标应用，等主窗口出现再 attach**（启动期 GUI 繁忙，注入/操作易 2004 超时）
3. `qt_attach <pid>`（pid 用 `qt_list_processes` 查询）→ `qt_list_sessions` 确认 connected

## 标准工作流

1. `qt_snapshot(max_depth=1, detail=extended)` → 读取快照资源（`qt-commander://sessions/...`）查看 UI 树
2. `qt_find_element` 定位目标（QML 自定义组件用 object_name/properties 组合；type_inherits 不匹配 QML 类型名）
3. **立即使用返回的 id 执行操作**——每次快照/find 刷新都会使旧 id 失效
4. 每步操作前重新 find（id 生命周期短）
5. 操作后 `qt_screenshot` 佐证

## 操作纪律（必须遵守）

1. **鼠标点击一律走真实输入管道**：`qt_mouse_click_region`（元素中心）或 `qt_mouse_click_at`（精确坐标）——经 Qt 真实事件分发 + hit testing，与人点击一致（QML MouseArea、焦点切换正常触发）。**不区分 QWidget/QML**。
2. `qt_mouse_click`（直接向控件投递）仅作调试后备，日常操作不用。
3. 键盘输入 `qt_keyboard_input` 本身已是真实输入；必须用最新 id，过期 id 会报错（`Element not found: id=N`），不会静默输入。
4. **2004 超时 ≠ 失败**：请求可能仍会执行（at-least-once）。**不要盲目重试**副作用操作（点击/输入/设属性），先 snapshot 验证状态再决定。
5. 优先真实输入；避免 `qt_set_property`/`qt_call_method`（破坏性、绕过真实交互路径）。
6. 重建库前必须 `qt_detach(purge=True)`（DLL 被目标进程锁文件，构建会失败）。
7. 2007 = 明确未执行，可安全重试（与 2004 不同，见 references/error-codes.md）。

## 验证闭环模板

操作前 snapshot 基线 → 执行操作 → 操作后 snapshot/find 断言状态变化 → 截图佐证 → 汇报

## 参考

- `references/error-codes.md` — 错误码对照表（含 2007 vs 2004 重试语义）
- `references/build-params.md` — 构建参数与目标进程匹配表
- `references/troubleshooting.md` — 故障排查决策树
