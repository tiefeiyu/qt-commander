# qt-commander 使用注意

日期：2026-11-06  
适用版本：`9c0b5f5` 及以后

原来的优化项已在 `9c0b5f5` 修复并在 QGC 上实测（点击路由、toolchain 识别、`qt_call` 字符串参数、窗口坐标点击、`qt_status` artifacts）。日常使用只剩下面两点需要记住。

## 1. 改了 C++ 后要重建注入库并重启目标

`src/library/server.cpp` 等 C++ 代码改动后，仅重启 MCP 不会更新已经注入目标进程的运行时。

1. 停止目标进程；
2. 用 `qt_build`（msvc / Qt 5.15.2 / Debug）重建 `qt_commander_inject.dll`；
3. 重新启动目标进程并 `qt_attach`。

否则 attach 到的仍是旧 DLL，症状看起来像“修复没生效”。2026-11-06 实测踩过一次。

## 2. 弹层根对象按 `qml_id` 查不到

`indicatorPopup`、`toolSelectDialog` 这类 Popup 打开时，`qt_query` 用 `qml_id` 查询返回 0。改用弹层内的文字定位：

```text
qt_query { where: { text: { contains: "网络式远程 ID 报送" } } }
```

或直接查询弹层里的具体子节点。

注意：QGC 的 `QGCPopupDialog` 是 Qt Quick Controls 2 的 `Popup`，默认渲染在主窗口的 Overlay 里，**不是独立窗口**——弹层打开时 `qt_windows` 仍然只返回主窗口。因此不能按 window 处理；弹层内容在 item 树里，用文字或子节点定位。如果遇到真正拥有独立窗口的弹层（`Qt.Popup` window），`qt_windows` 会把它列出来（`popup` 字段为 true），那时可以按窗口坐标点击。
