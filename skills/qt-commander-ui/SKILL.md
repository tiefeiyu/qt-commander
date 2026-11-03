---
name: qt-commander-ui
description: 驱动已运行的 Qt 应用完成 UI 验证闭环（QWidget/QML 的定位、点击、输入、读取、截图）。当任务需要操作真实 Qt 界面并留下证据时使用；不用于纯后端/日志验证或进程启停。
allowed-tools: mcp__qt-commander__*
---

# qt-commander-ui

这个 skill 只管**怎么用 qt-commander 完成一次可靠的 UI 验证**，不重复工具参数表。
工具的完整 schema 以 MCP 返回为准。

## 先判断

- 目标进程已经运行：从 `qt_status` / `qt_windows` / `qt_attach` 开始。
- 目标进程还没运行：先用项目自身的启动方式启动，等主窗口出现后再 attach。
- 只需要看代码/日志：不要用这个 skill。
- 需要进程管理：不要用这个 skill。

## 标准闭环

1. **基线**
   - `qt_status` 找到 descriptor；必要时 `qt_windows` 选窗口。
   - `qt_attach(descriptor=...)` 或 `qt_attach(pid=..., mode="injection")`。
   - 先截一张图，记录当前窗口/视图。

2. **定位**
   - `qt_query`，优先用 `qml_id`、`object_name`、`text`、`class`。
   - 不传 `props` 时节点已有 `text/visible/enabled`；需要更多属性再显式传。
   - target 可以是：
     - 查询返回的节点 id；
     - 字符串形式的 QML `qml_id`；
     - `{"qml_id": ...}` / `{"text": ...}` / `{"class": ...}` / `{"object_name": ...}`；
     - 组合条件，例如 `{"qml_id":"buttonTemplate","text":"飞行"}`。
   - 多个匹配时先看 `E_AMBIGUOUS.details.candidates`，按 `visible`、`class`、`rect`、`nth` 选目标；不要猜。
   - 如果目标在隐藏视图里，默认会被拒绝；只有明确要操作隐藏对象时才传 `include_hidden=true`。

3. **执行**
   - 输入/点击/键盘：`qt_act`，走真实输入。
   - 读属性：`qt_get`。
   - 写属性：`qt_set`；属性 setter 不是 `qt_call` 方法。
   - 调用槽/`Q_INVOKABLE`：`qt_call`；先用 `qt_methods` 看签名，重载用 `signature`。
   - 多个动作/写入/读取/恢复：`qt_batch`。
   - 窗口：`qt_act(target=<window id>, action="restore"|"minimize"|"set_geometry")`；`set_geometry` 后检查 `applied` 和 `resolved.rect`。

4. **验证**
   - 操作后重新 `qt_get` / `qt_query` 或截图，确认状态真的变了。
   - 截图标注优先用 `legend` 里的 `id`，不要用图上的编号当 target。
   - 标注很大时用 `annotate_offset` 翻页；不要假设第一页覆盖全部。
   - 需要源码位置时用 `qt_source`；qrc/嵌入式 QML 可传 `source_dir`。

5. **收尾**
   - 关闭自己打开的弹层/临时视图。
   - `qt_detach`；如果目标库要重建，先 detach，避免 DLL 被占用。

## 错误处理

- `E_AMBIGUOUS`：候选在 `details.candidates`，选 `nth` 重试。
- `E_HIDDEN`：目标存在但全部隐藏；确认是否应操作隐藏对象，再决定 `include_hidden=true`。
- `E_NOT_FOUND`：重新 query；不要猜 id。
- `E_NOT_EDITABLE`：目标不是可编辑控件；改找输入框或可编辑子节点。
- 超时：不要盲目重试副作用操作；先查状态/截图，再决定。

## QML 注意

- 纯 QML 应用是 QQuickWindow，不是 QWidget；用 `qml_id`/`qml_type` 定位。
- 重复 delegate 的 `qml_id` 很常见；组合 `text`/`class`/`objectName` 后再选 `nth`。
- 文本可能只命中内部 `QQuickText`；qt-commander 会自动折叠到可点击祖先，但仍应截图确认没有误点。
- 完全自绘且无 text/objectName 的控件，最后才用坐标；优先找它的父级可点击容器。
- QML 的 `visible` 是有效可见性；子标签看着有文字不代表父按钮可见。

## 安全

- `qt_act`、`qt_set`、`qt_call` 都可能改变应用状态；先确认目标，再做。
- 不连接、不解锁、不起飞/降落、不上传任务、不改持久设置，除非任务明确要求。
- 对真实应用不要用 `include_hidden=true` 绕过保护来“碰运气”。
