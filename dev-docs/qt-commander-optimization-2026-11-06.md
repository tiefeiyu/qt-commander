# qt-commander 可用性优化建议

日期：2026-11-06  
来源：QGroundControl（Qt 5.15.2 / MSVC / Debug）联调会话  
qt-commander 版本：`83dba38 feat(artifacts): use one nested target-keyed root`  
验证平台：Windows 11，Qt 5.15.2 msvc2019_64，VS2022 14.44，目标进程为 Debug 构建

本文按优先级列出实测遇到的问题、根因定位、修复建议和复现方案。除特别标注外，问题均在本次会话中实际复现过。

## 优先级总览

| # | 问题 | 影响 | 优先级 |
|---|---|---|---|
| 1 | QML 点击被发到祖先中心，点错控件或点空 | 交互类自动化不可信；需要手动找 MouseArea 节点绕过 | P0 |
| 2 | 目标进程 toolchain 识别丢失（null），artifact 明明存在却无法 attach | 未自带 MSVC 运行库的目标全部无法注入 | P1 |
| 3 | attach 失败只返回 “injection DLL not found”，不返回查找条件与候选 | 用户/代理必须读源码才能判断该建哪个 artifact | P1 |
| 4 | `qt_call` 无法给 QML 函数传字符串参数 | QML 状态机/动作无法脚本化调用 | P2 |
| 5 | 窗口目标不支持按坐标点击；`qt_status` 不列 artifacts；弹层根对象查不到 `qml_id` | 排障与兜底能力不足 | P3 |

---

## 1. P0：QML 点击坐标被祖先中心覆盖

### 现象

在 QGC 工具栏点击 UOM 状态指示器（一个 `Item`，内部是 `Column > Image`，点击区是与 `Column` 平级的 `MouseArea`，整体位于工具栏 `QGCFlickable` 内）：

- `qt_query { object_name: "ridUomStatusIndicator" }` 拿到指示器节点，`qt_act click` 该节点：**无任何反应**。
- `qt_act click` 指示器的 `QQuickImage` 子节点：**仍无反应**。
- `qt_query` 在指示器范围内找到 `QQuickMouseArea` 节点，`qt_act click` 该 MouseArea：**弹层正常打开**。

第二个同类现象（同一次会话，打开“选择工具”弹层）：点击 `qml_id=settingsButton`（文本“应用程序设置”，`SubMenuButton`）的节点，打开了**“分析工具”**；改点它内部的 `titleBar` 标签（同一个按钮内的 `QGCLabel`）则能正确打开“应用程序设置”。该现象与根因的关系尚未逐行确认，但符合“点击点被换成了某个祖先中心”的特征。

### 根因

`src/library/server.cpp:4619-4648`：

```cpp
QQuickItem* clickItem = item;
for (QQuickItem* candidate = item; candidate;
     candidate = candidate->parentItem()) {
    if ((candidate->acceptedMouseButtons() & Qt::LeftButton) ||
        candidate->inherits("QQuickAbstractButton") ||
        candidate->inherits("QQuickMouseArea")) {
        clickItem = candidate;
        break;
    }
}
QQuickItem* clickTarget = clickItem;
const QPointF point =
    clickTarget->mapToScene(clickTarget->boundingRect().center());
```

问题有两层：

1. **坐标被替换**：找到可接受的祖先之后，事件坐标改用该祖先的中心，而不是原始目标项的中心。
2. **找祖先本身会跨过真正的点击区**：Qt Quick 中 `QQuickItem` 默认 `acceptedMouseButtons` 是 `Qt::NoButton`（见 Qt 源码 `qquickitem.cpp:7290` 注释），普通 `Item`/`Column`/`Image`/`Loader` 都会继续向上爬；而 QML 最常见的写法恰恰是“视觉项 + 平级覆盖的 MouseArea”，MouseArea 是**兄弟**，不是祖先，永远找不到。于是爬到了 `Flickable` 这种真正接受鼠标的容器，用它的中心点击——离目标十万八千里。

这也解释了为什么直接点 MouseArea 节点能成功：MouseArea 自己就是接受者，坐标等于它自己的中心。

### 建议修法

事件坐标**始终使用原始目标项**在场景中的中心；向上找祖先只用于确定聚焦对象（甚至可以直接用原目标），不要用于改坐标：

```cpp
QQuickItem* focusItem = item;
for (QQuickItem* candidate = item; candidate;
     candidate = candidate->parentItem()) {
    if ((candidate->acceptedMouseButtons() & Qt::LeftButton) ||
        candidate->inherits("QQuickAbstractButton") ||
        candidate->inherits("QQuickMouseArea")) {
        focusItem = candidate;
        break;
    }
}
// Key point: the event position comes from the requested item, not from an
// ancestor. Qt delivers it to the topmost MouseArea/button at that point.
const QPointF point = item->mapToScene(item->boundingRect().center());
focusItem->forceActiveFocus();
// ... send press/release to `window` at `point` as today
```

退化情况：原始项 `width`/`height` 为 0 或 `boundingRect()` 为空时，再回退到 `focusItem` 的中心。

这个改法同时覆盖两种 QML 结构：

- 图标被平级 MouseArea 覆盖：事件点落在图标上，窗口路由给覆盖它的 MouseArea；
- 图标是某个 Button 的子项：事件点落在图标上，窗口路由给 Button 的 MouseArea/AbstractButton。

不需要识别“谁应该接收”，这正是真实鼠标事件的语义。

### 复现方案 A：QGC（已实测）

1. QGC 连接一架飞机（应用设置 → 模拟链接 → PX4 载具），工具栏出现 UOM 云朵指示器。
2. `qt_query { "object_name": "ridUomStatusIndicator" }`，记下节点 id。
3. `qt_act { "action": "click", "target": "<该 id>" }` → 无反应。
4. `qt_query { "scope": { "id": "<该 id>" }, "where": { "class": { "contains": "MouseArea" } } }`，记下 MouseArea id。
5. `qt_act { "action": "click", "target": "<MouseArea id>" }` → 弹层打开。

### 复现方案 B：最小 Qt Quick 工程（建议作为回归测试）

结构刻意复刻 QGC 工具栏：`Flickable` 里放三行，每行的视觉根节点不处理点击，点击由平级 `MouseArea` 处理。点击第三行的根节点时，当前实现会爬到 `Flickable`，用它的中心点击，命中第二行。

工程源码见附录 A。步骤：

1. `qt_build` 构建（参数见附录 A 末尾）。
2. `windeployqt --debug` 部署 Qt 运行库与 platforms 插件（否则会报 “no Qt platform plugin”）。
3. 启动 `qtc_click_repro.exe`，`qt_attach`。
4. `qt_query { "object_name": "row-3" }`，`qt_act click` 该节点。
5. 读取 `object_name=resultLabel` 的文本：
   - 修复前：`clicked: row-2`（错误）；
   - 修复后：`clicked: row-3`（正确）。
6. 再点 `row-2`、`row-1`，以及每行内部的 `Text`，都应命中对应行。

### 验收标准

- 点击任意视觉子项，命中包含该点、处于最上层的 MouseArea/Button；
- 点击行为与真实鼠标点击同点一致；
- 现有 QWidget 分支行为不变。

---

## 2. P1：目标 toolchain 识别丢失，导致无法 attach

### 现象

同一个 `qt_attach` 错误在两种情况下出现：

```
E_INJECTION_FAILED: injection DLL not found
hint: build qt_commander_inject or set QT_COMMANDER_INJECT_DLL
```

情况一：机器上只有 mingw release artifact，目标是 msvc debug —— 可以理解，但信息不足（见第 3 节）。

情况二（本次新发现）：**正确的 msvc debug artifact 存在**，目标也是 Qt 5.15.2 MSVC Debug，仍然报同样的错。`qt_target_info` 显示：

```json
{
  "qt_version": "5.15.2",
  "debug": true,
  "toolchain": null,
  "compiler": null,
  "compiler_version": null,
  "evidence": [
    "C:\\Software\\Qt\\5.15.2\\msvc2019_64\\bin\\Qt5Core.dll",
    "C:\\Software\\Qt\\5.15.2\\msvc2019_64\\bin\\Qt5Cored.dll"
  ]
}
```

`artifacts._matches()` 要求 `manifest.compiler.family == target.toolchain`，`"msvc" != null`，因此候选为空。

把 `msvcp140d.dll`、`vcruntime140d.dll`、`Qt5Cored.dll` 等复制到 exe 同目录后，`qt_target_info` 变成 `"toolchain": "msvc"`，attach 成功。

### 根因

`qt_commander/target.py:179-229` 的 `_scan_runtime_dirs()`：

- 逐目录扫描；一旦某个目录里找到 `Qt5Core*.dll`，**立即返回该目录的结果**；
- compiler/toolchain 只从“同一个目录”里的 `msvcp*` / `vcruntime*` / `libstdc++` 推断；
- 之前的目录里已经推断出的 `fallback_toolchain` 被丢弃。

典型布局是“Qt 在共享安装目录、MSVC 运行库由系统/安装器提供”，Qt 的 bin 目录里并没有 `msvcp140d.dll`，于是 toolchain 恒为 null。exe 同目录或进程模块里其实有 msvcp，但扫描没有合并证据。

### 建议修法

按可靠性排序：

1. **从 PE 导入表推断**（最可靠）：读目标 exe/模块的 import table，出现 `msvcp*.dll`/`vcruntime*.dll` → msvc，出现 `libstdc++-6.dll`/`libgcc_s_*.dll` → mingw。与 Qt DLL 放在哪个目录无关。
2. **合并所有目录的证据**：找到 Qt 的目录返回时，使用 `toolchain = toolchain or fallback_toolchain`，并把所有目录的 evidence 合并；不要因为先遇到 Qt 目录就丢弃之前的结论。
3. **从模块列表推断**：`EnumProcessModules` 取已加载模块，查找 msvcp/vcruntime 与 libstdc++/libgcc。
4. 作为最后兜底：target.toolchain 仍为 null 且恰好只有一个 artifact 满足 os/arch/Qt/debug 时，可用它并在结果里标记 `"toolchain_inferred": true`（有注入错误 CRT 的风险，需谨慎）。

### 复现方案

1. 用一个 MSVC Debug 构建的 Qt Quick 程序，Qt 运行库放在 PATH 的共享目录，exe 目录只留 exe。
2. `qt_target_info(pid)` → `toolchain: null`。
3. `qt_attach(pid, mode="injection")` → `E_INJECTION_FAILED`，即使 `artifacts/windows-x64/msvc-19.44/qt-5.15.2-debug` 完整存在。
4. 把 `msvcp140d.dll` / `vcruntime140d.dll` 复制到 exe 目录，重启进程 → `toolchain: "msvc"`，attach 成功。

### 验收标准

- Qt DLL 位于任意目录时，MSVC/MinGW 目标都能正确识别；
- 识别结果写入 `qt_target_info` 的 evidence，便于排障；
- 现有 `tests/unit_server/test_target.py` 增加上述两种布局的用例。

---

## 3. P1：attach 失败信息缺少 artifact 诊断

### 现象

`qt_attach` 失败只返回：

```json
{"code":"E_INJECTION_FAILED","message":"injection DLL not found",
 "hint":"build qt_commander_inject or set QT_COMMANDER_INJECT_DLL"}
```

不包含目标元数据、期望的 artifact 路径、实际存在哪些 artifact、为什么没匹配上。本次只能读 `artifacts.py` / `server.py` 才知道目录布局与匹配条件。

### 根因

`qt_commander/inject.py:40-52` 的 `default_dll_path()` 丢弃了 `artifacts.find()` 的失败结果：

```python
found = _artifact_for_pid(pid)
if not found.get("ok"):
    return None
```

`artifacts.find()` 已经返回 `searched`（见 `artifacts.py:170-177`），但 `server.py:207-214` 只看到 `None`。

### 建议修法

- 让 `default_dll_path()` 返回 `(path, detail)` 或抛出带 detail 的异常；
- `_inject_target()` 把 detail 放进错误对象，例如：

```json
{
  "code": "E_INJECTION_FAILED",
  "message": "no injector artifact matches the target",
  "target": {"os":"windows","arch":"x64","qt_version":"5.15.2",
             "debug":true,"toolchain":"msvc","compiler_version":null},
  "expected": "artifacts/windows-x64/msvc-19.44/qt-5.15.2-debug",
  "searched": "D:/Develop/qt-commander/artifacts",
  "available": [
    "windows-x64/mingw-gcc-13.1/qt-5.15.2-release (compiler mingw, qt release)",
    "windows-x64/msvc-19.44/qt-5.15.2-debug (toolchain unknown for target)"
  ],
  "hint": "build qt_commander_inject for this target or set QT_COMMANDER_INJECT_DLL"
}
```

- `qt_status` 增加 `artifacts` 数组（路径、manifest 摘要、是否当前可用），这样不读源码也能判断。

### 验收标准

- 错误响应能直接回答“缺哪个矩阵的 artifact、为什么现有 artifact 不匹配”；
- 不再需要 `QT_COMMANDER_INJECT_DLL` 才能获得诊断。

---

## 4. P2：`qt_call` 无法给 QML 函数传字符串

### 现象

QML 里声明：

```qml
function _openActivationDialog(mode) { ... }
```

调用：

```json
{"method":"_openActivationDialog","args":["deregister"]}
```

返回：

```
E_INVALID_ARGUMENT: cannot convert QML argument 0 to QVariant
```

### 根因

`src/library/server.cpp:3208-3233` 对每个参数执行：

```cpp
QVariant value = args.at(i).toVariant();
const int typeId = chosen.parameterType(i);
...
} else if (value.userType() != typeId && !value.convert(typeId)) {
    // error "cannot convert QML argument i to <type>"
}
```

QML 声明的函数在元对象里参数类型是 `QVariant`；`QVariant::convert(QMetaType::QVariant)` 失败（对自身类型没有转换路径），于是报错。整型/枚举已经在上面的分支里处理，字符串参数则完全无法传入。

### 建议修法

- `typeId == QMetaType::QVariant`：直接保留 `value`，不做 `convert`；
- 参数类型是 `QJSValue`：构造 `QJSValue(value)`（或 `engine->toScriptValue(value)`）；
- 参数类型是 `QVariantMap`/`QVariantList`/`QString` 等：现有 `convert` 可保留；
- 增加一个参数转换的单元测试（QML function 带 string/int/bool 参数）。

### 验收标准

- 带字符串参数的 QML 函数可以通过 `qt_call` 调用；
- 参数个数/签名不匹配时仍返回 `E_NOT_FOUND` 与候选签名。

---

## 5. P3：其他小项

1. **窗口目标只支持 key**（`server.cpp:1559-1566`）。建议允许按窗口绝对坐标点击，作为 QML 节点解析失败时的兜底：
   `qt_act { target: {class:"QQuickWindow"}, action:"click", position:{x,y} }`。
2. **`qt_status` 不列 artifacts**。建议加字段，便于判断注入器是否齐全（与第 3 节配套）。
3. **弹层根对象查不到 `qml_id`**。`indicatorPopup` / `toolSelectDialog` 打开时用 `qml_id` 查询返回 0，只能按文本搜索。Popup 不是 `QQuickItem`，当前树只注册 item；建议把 Popup 对象也注册为可查询节点，或在其 contentItem 上保留可查的 objectName。
4. **文档**：README/工具描述应写清：
   - artifact 矩阵 `artifacts/<os>-<arch>/<compiler-version>/qt-<version>-<debug|release>/manifest.json`；
   - `qt_build` 的 `qt_env` 必须是 `qt_environments()` 返回的 kit 路径；
   - `toolchain` 默认是 mingw（对 Windows 应用经常需要显式传 msvc）；
   - MSVC 需要 `vcvars_path`。

---

## 6. 建议的回归测试

1. **QML 点击**：在 `tests/test-apps/qmlwindow` 增加“Flickable + 三行 + 平级 MouseArea”的 fixture（附录 A 可直接并入），断言点击 `row-3` 根节点后 `resultLabel` 为 `row-3`。这是本次最强的回归保护。
2. **target 扫描**：在 `tests/unit_server/test_target.py` 增加两个目录布局：Qt 与运行库同目录、Qt 与运行库分离；断言 toolchain 分别为 msvc 且不因目录顺序变化。
3. **artifact 诊断**：构造“只有 mingw release artifact”的临时目录，断言 `find()` 返回 `available`/`searched`，attach 错误包含这些字段。
4. **qt_call 参数**：QML fixture 加 `function echo(text) { return text }`，断言 `qt_call` 返回相同字符串。

---

## 7. 本次验证记录

已验证：

- QGC 中 `qt_act click` 指向指示器 Item/Image 无反应，指向其 MouseArea 正常打开弹层；
- 点击 `settingsButton` 节点打开“分析工具”，点击其内部 `titleBar` 标签打开“应用程序设置”；
- `qt_call` 传 `"deregister"` 返回 `cannot convert QML argument 0 to QVariant`；
- 目标 toolchain 为 null 时 attach 失败；复制 MSVC 运行库与 Qt DLL 到 exe 同目录后 toolchain=msvc、attach 成功；
- `qt_build` 用 msvc/Qt 5.15.2 Debug 构建注入器成功，attach QGC 成功；
- 最小 Qt Quick 复现工程能构建、启动；其部署需要 `windeployqt`（否则缺 platforms 插件）。

未验证（仅代码审阅）：

- `qt_build` 的 compiler 版本选择问题：`server.py:546-555` 取“第一个带 version 的 MSVC 工具链”写 manifest，而实际编译使用传入的 `vcvars_path`。若机器同时装有 VS2022 与 VS2026，且传入后者，artifact 会标成 19.44、实际用 14.51，后续按 compiler_version 匹配可能失败。建议从 `vcvars_path` 反查工具链版本，或在结果中同时返回两者。
- 附录 A 的 fixture 尚未在本机完整跑通点击断言（只验证到构建/启动）。

---

## 附录 A：最小复现工程

目录结构：

```
qtc-click-repro/
  CMakeLists.txt
  main.cpp
  qml.qrc
  Main.qml
```

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(qtc_click_repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 COMPONENTS Core Gui Qml Quick REQUIRED)

add_executable(qtc_click_repro main.cpp qml.qrc)
target_link_libraries(qtc_click_repro PRIVATE
    Qt5::Core Qt5::Gui Qt5::Qml Qt5::Quick)
```

`main.cpp`：

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
```

`qml.qrc`：

```xml
<RCC>
    <qresource prefix="/">
        <file>Main.qml</file>
    </qresource>
</RCC>
```

`Main.qml`：

```qml
import QtQuick          2.15
import QtQuick.Window   2.15
import QtQuick.Layouts  1.15

// Mirrors the QGC toolbar structure: each row is a plain visual Item with a
// sibling MouseArea, and the whole column sits inside a Flickable (which does
// accept mouse buttons). Clicking row-3's root must hit row-3's MouseArea.
Window {
    id:         window
    width:      420
    height:     340
    visible:    true
    color:      "#202020"

    Text {
        id:                 result
        objectName:         "resultLabel"
        text:               "clicked: none"
        color:              "white"
    }

    Flickable {
        objectName:     "flick"
        anchors.top:    result.bottom
        width:          parent.width
        height:         260
        contentWidth:   column.width
        contentHeight:  column.height
        clip:           true

        Column {
            id:         column
            spacing:    8

            Repeater {
                model: ["row-1", "row-2", "row-3"]

                Item {
                    objectName: "row-" + modelData
                    width:      240
                    height:     64

                    Rectangle {
                        anchors.fill: parent
                        color:        "#333333"
                    }

                    Text {
                        anchors.centerIn: parent
                        text:             modelData
                        color:            "white"
                        font.pixelSize:   18
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked:    result.text = "clicked: " + modelData
                    }
                }
            }
        }
    }
}
```

构建与运行（Windows / Qt 5.15.2 msvc2019_64）：

```text
qt_build(
  source_dir   = "D:/tmp/qtc-click-repro",
  build_dir    = "D:/tmp/qtc-click-repro/build",
  qt_major     = 5,
  qt_env       = "C:/Software/Qt/5.15.2/msvc2019_64",
  toolchain    = "msvc",
  arch         = "x64",
  build_type   = "Debug",
  vcvars_path  = "C:/Software/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat")

windeployqt --debug D:/tmp/qtc-click-repro/build/qtc_click_repro.exe
```

说明：`windeployqt` 会同时解决两个问题——把 Qt DLL 放到 exe 同目录（使 toolchain 识别命中）并部署 `platforms/qwindowsd.dll`。不部署会报 “no Qt platform plugin could be initialized”。

断言步骤：

```text
qt_query { where: { object_name: "row-3" } }        -> id
qt_act   { action: "click", target: <id> }
qt_get   { target: { object_name: "resultLabel" }, props: ["text"] }
```

修复前期望得到 `clicked: row-2`（复现成功），修复后为 `clicked: row-3`。
