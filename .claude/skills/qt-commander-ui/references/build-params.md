# qt_build 构建参数与目标进程匹配表

qt_build 产物注入失败（错误码 2002）时，九成是构建参数与目标进程不匹配。**每次构建前用 `qt_list_processes` 查看目标进程的 qt_version/arch/bitness，逐项核对。**

| 参数 | 必须匹配项 | 说明 |
|---|---|---|
| qt_major | 目标进程 Qt 大版本 | 5 或 6；跨大版本注入必然失败 |
| toolchain | msvc / mingw | 与目标进程所用工具链一致 |
| build_type | Debug / Release | Debug 构建的目标进程必须用 Debug 库，反之亦然 |
| 位数 | 64 / 32 | 与目标进程架构一致 |

## 核对步骤

1. `qt_list_processes` → 记录目标进程的 `qt_version` / `arch` / `bitness`
2. 按上表逐项设置 qt_build 参数
3. 构建产物在 `<workspace>/.qt-commander/bin/`；注入库依赖闭包必须完整（如 QML 库缺 Qt5Quick/Qt5Qml/Qt5QuickWidgets 会导致 attach 失败）
4. attach 失败（2002）时回查本表；参数无误则删除 `.qt-commander/build_manifest.json` 后重试（MCP 进程内旧逻辑可能算错源码哈希）
