# 故障排查决策树

按症状定位问题，从最可能的原因开始。

## attach 失败（2002）

构建参数与目标进程匹配？→ `qt_list_processes` 核对 qt_major/toolchain/build_type/位数（见 build-params.md）
→ 构建产物存在？→ `.qt-commander/bin/` 下 libqt-commander.dll 与 qt-injector.exe
→ MCP 进程逻辑过期？→ 重启 MCP 服务器（源码变更后必须重启）

## snapshot 超时

应用繁忙 → 等 5s 重试一次
仍超时 → 应用可能卡死 → 检查应用进程状态，考虑重启应用

## find 不到元素

- `include_hidden` 未开？→ 隐藏元素默认不匹配，按需开启
- objectName 拼写错误？→ 对照快照中的实际 objectName
- 页面未加载？→ 等待页面加载完成再 find
- 自定义 QML 组件？→ 用 object_name/properties 匹配，type_inherits 不匹配 QML 类型名

## 点击无效

- 元素可见且未被遮挡？→ 遮挡时坐标点击会命中遮挡物（跨窗口遮挡是设计行为）
- 坐标是否命中元素？→ click_region 用元素中心；被遮挡/命中不准时用 click_at 精确定位重试
- 仍无效 → 确认元素是真实交互组件（如 QML 组件需其自身有 MouseArea 处理点击）

## 键盘输入未落

- 目标未聚焦 → 先 click_region 点击目标使其获得焦点，再输入
- 输入进了错误控件 → 元素 id 过期？重新 find；QML 应用无 focus widget，`element_id=0` 回退不可用

## 元素 id 过期报错

每次 find/snapshot 后 id 全部失效 → 重新 find 获取新 id，绝不复用旧 id

## 2004 超时后状态异常

先 snapshot 断言当前状态（请求可能已执行）→ 按需补操作，不盲目重试

## 目标进程崩溃

用 cdb 附加复现：`cdb.exe -p <pid>`（符号路径指向目标应用的 Qt bin 目录）
