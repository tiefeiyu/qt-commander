# qt-commander MCP 注入/重建流程反馈

> 日期：2026-11-10
> 场景：对 RealEvo-Qt5SylixOS-QtCreator（Qt 5.15.2 / MSVC / Debug / x64）做运行时 UI 自动化

## 结论

正常 attach 可用；**注入失败后的诊断和重建注入器**这一层信息不足，导致必须去读 README、CMakeLists、artifact manifest 和项目 memory 才能恢复。

## 复现过程

1. `qt_attach(pid=..., mode="injection")` 返回：

   ```json
   {"ok":false,"error":{"code":"E_INJECTION_FAILED","message":"timeout waiting for QCoreApplication","retryable":false}}
   ```

2. `qt_target_info(pid=...)` 能正确识别目标：

   ```text
   qt_major = 5
   qt_version = 5.15.2
   debug = true
   toolchain = msvc
   compiler_version = 14.44.35207
   arch = x64
   ```

3. `qt_environments()` 能列出：

   - Qt 5.15.2 → `C:/Software/Qt/5.15.2/msvc2019_64`
   - MSVC 14.44 → VS2022 `cl.exe`
   - `vcvars64.bat` 路径

4. 但没有任何工具告诉调用方：

   - 目标进程里已经加载了旧的 `qt_commander_inject.dll`
   - 这个 DLL 来自哪个 artifact/build id
   - 需要重启目标进程才能重新注入
   - `qt_build` 应该用哪个源码目录、构建目录和参数

5. 最终通过 PowerShell 才发现旧 DLL 已加载：

   ```powershell
   Get-Process -Id <pid> | ForEach-Object {
       $_.Modules | Where-Object { $_.ModuleName -match 'qt_commander' }
   }
   ```

6. 通过阅读以下文件才确定重建方式：

   - `D:/Develop/qt-commander/README.md` 的 Injection artifacts / qt_build 段落
   - `CMakeLists.txt`
   - `src/CMakeLists.txt`
   - `src/injector/CMakeLists.txt`
   - 现有 artifact 的 `manifest.json`
   - 本地 memory 中的 `qt_build` 调用记录

## 本次实际使用的重建参数

```text
source_dir   = D:/Develop/qt-commander
build_dir    = D:/Develop/qt-commander/build/windows-x64_msvc-19.44_qt-5.15.2-debug
qt_major     = 5
toolchain    = msvc
arch         = x64
build_type   = Debug
qt_env       = C:/Software/Qt/5.15.2/msvc2019_64
vcvars_path  = C:/Software/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat
extra_args   = ["-DQT_MAJOR_VERSION=5", "-DQt5_DIR=C:/Software/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5"]
```

`qt_build` 最终成功，并返回了：

```text
artifact_dir = D:/Develop/qt-commander/artifacts/windows-x64/msvc-19.44/qt-5.15.2-debug
manifest     = .../manifest.json
build_id     = qt5.15.2/msvc-x64/Debug
```

这些返回字段很有用，但参数发现过程仍然依赖人工读文件。

## 建议的工具改进

### P0：让 `qt_attach` 失败可直接恢复

增加结构化错误字段，区分不同失败原因：

```json
{
  "code": "E_INJECTION_FAILED",
  "reason": "injector_already_loaded",
  "target": {
    "pid": 1234,
    "qt_version": "5.15.2",
    "toolchain": "msvc",
    "debug": true,
    "arch": "x64"
  },
  "loaded_injector": {
    "path": "D:/Develop/qt-commander/artifacts/.../qt_commander_inject.dll",
    "build_id": "qt5.15.2/msvc-x64/Debug"
  },
  "expected_artifact": "D:/Develop/qt-commander/artifacts/...",
  "restart_required": true,
  "hint": "The target already has an injector DLL loaded. Restart the target process before reinjecting."
}
```

至少应区分：

- `injector_already_loaded`
- `artifact_missing`
- `artifact_mismatch`
- `timeout_waiting_qcoreapp`
- `target_not_ready`

### P0：让 `qt_build` 能按目标自动推导参数

建议增加 `for_pid` / `target_pid`：

```text
qt_build(for_pid=1234)
```

服务端自动从 `qt_target_info` 推导：

- `qt_major`
- `toolchain`
- `arch`
- `build_type`（Debug/Release）
- Qt kit 路径
- `vcvars_path`

并允许省略 `source_dir`，默认使用 MCP server 自己的 qt-commander 源码。返回值建议包含：

```text
artifact_dir
manifest
build_id
restart_required
```

### P1：增加 artifact / 注入器状态查询

建议新增工具或扩展现有工具：

- `qt_artifacts`：列出当前所有 injector artifact，以及和目标进程的匹配状态
- `qt_target_info` 增加：

  ```text
  loaded_injector: { loaded, path, build_id, matches_artifact }
  injection_state: none | loaded | mismatch | stale
  restart_required: true/false
  ```

- `qt_status` 对指定 PID 返回已加载的注入器 DLL 和 session 状态

### P1：`qt_environments` 补充 qt-commander 自身信息

`qt_environments` 目前只有 Qt kit 和 toolchain。建议增加：

```text
qt_commander_source_dir
artifact_root
suggested_build_args:
  qt_major: 5
  toolchain: msvc
  build_type: Debug
  qt_env: C:/Software/Qt/5.15.2/msvc2019_64
  vcvars_path: ...
```

或者直接返回一段可执行的 `qt_build` 参数示例。

### P2：工具描述补充关键约定

建议在 MCP 工具描述中直接写明：

- artifact 根目录和命名规则
- `qt_build` 的 `source_dir` 默认值/示例
- 注入失败后如果旧 DLL 已加载，必须重启目标进程
- Debug/Release 必须与目标进程匹配

## 最小改进集

如果只做最小改动，建议优先：

1. `qt_attach` 失败时报告“已加载的 injector DLL”和 `restart_required`
2. `qt_build` 支持 `for_pid` 自动推导参数，并允许省略 `source_dir`
3. `qt_target_info` 返回 `loaded_injector` 和 `matching_artifact`

这三项可以覆盖本次实际遇到的完整恢复路径。

## 2026-11-10 真机验证补充：debug/release 误判

目标：`RealEvo-Qt5SylixOS-QtCreator.exe`（Qt 5.15.2 / MSVC 14.44 / x64 / Release）。

实际现象：

- `dumpbin /dependents` 显示 exe 只依赖 `Qt5Core.dll`（release）和 release CRT。
- 但 `qt_target_info` 返回 `debug: true`，evidence 同时列出 PATH 中的 `Qt5Core.dll` 和 `Qt5Cored.dll`。
- 结果 `qt_attach` 自动选择了 debug artifact；注入后进程加载 `Qt5Cored.dll`，等待 `QCoreApplication` 超时。
- 重启目标、确认只加载 `Qt5Core.dll` 后，`qt_target_info` 仍返回 `debug: true`。

Release 构建本身是成功的：

```text
build_dir = D:/Develop/qt-commander/build/windows-x64_msvc-19.44_qt-5.15.2-release
CMAKE_BUILD_TYPE = Release
injector imports Qt5Core.dll / MSVCP140.dll
```

但 `qt_build(build_type=Release)` 返回的 `artifact_dir` 仍是：

```text
D:/Develop/qt-commander/artifacts/windows-x64/msvc-19.44/qt-5.15.2-debug
```

它尝试覆盖已被目标进程加载的 debug DLL，报：

```text
[WinError 32] 另一个程序正在使用此文件，进程无法访问。
```

本次的绕过方式：

1. 关闭目标进程，释放 debug DLL。
2. 用 `qt_build` 的 Release 构建目录，手动创建：

   ```text
   artifacts/windows-x64/msvc-19.44/qt-5.15.2-release/
     bin/qtc-inject.exe
     lib/qt_commander_inject.dll
     manifest.json   # qt.debug=false, build_type=Release
   ```

3. `qt_attach(pid=..., mode="injection", artifact_dir=".../qt-5.15.2-release")`，注入成功。

建议：

- `qt_target_info.debug` 以目标进程实际加载的 `Qt5Core.dll` / `Qt5Cored.dll` 为准，不要被 PATH 中同时存在的 debug DLL 或 debug CRT import 干扰。
- `qt_build` 的 `artifact_dir` 必须与 `build_id` 的 Debug/Release 一致；Release 构建不能写 debug artifact 目录。
- `qt_build` 增加 `for_pid`，自动使用目标实际的 debug/release 推导 artifact，并返回 `restart_required`。
- `qt_attach` 继续保留 `artifact_dir` 显式固定能力；本次正是靠它绕过误判。

## UI 自动化备注

- 对 `QRadioButton` 使用 `qt_set checked=true` 只会改变属性，不会触发 `QButtonGroup::idClicked`。对于监听 `idClicked` 的界面，需要用 `qt_call click()` 才会触发界面的参数变更逻辑。
- 这不是注入/构建问题，但会影响 UI 自动化的可靠性；建议在工具说明或示例中注明。
