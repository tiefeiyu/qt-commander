# qt-commander

Live Qt UI introspection and automation for AI agents.

qt-commander links a small in-process runtime into a Qt application, exposes
its live object tree over a local JSON-RPC connection, and drives it through
an MCP server. Queries are evaluated in the target process every time; there
are no snapshot files and no stale index of the UI.

## Install

> The first PyPI release is being prepared. Until it is out, install from
> source (option 2); the command line below already exists in this repository.

### Requirements

- Python 3.10+ and [uv](https://docs.astral.sh/uv/)
- A Qt 5 or Qt 6 target application. Verified on Qt 5.15.2 and Qt 6.8.3;
  other Qt 5.x/6.x releases are expected to work but are not yet validated.
- Windows or Linux
- Python is only needed for the MCP server; the target process does not need
  Python or gdb

### Option 1 — package plus prebuilt runtime

```bash
pip install qt-commander-mcp
qt-commander doctor            # what this machine offers
qt-commander install-artifacts # injector + runtime for this OS/architecture
```

`install-artifacts` downloads the release archive, verifies `SHA256SUMS`, and
unpacks it into the artifact root (`QT_COMMANDER_ARTIFACTS`, or a per-user
directory for a wheel install). No C++ compiler is needed.

### Option 2 — from source

```bash
git clone https://github.com/tiefeiyu/qt-commander.git
cd qt-commander
uv sync
uv run qt-commander doctor
```

Source-checkout artifacts built with `qt_build` or
`scripts/package_artifacts.py` land in `artifacts/` next to the checkout.

### Register it with your MCP client

Claude Code:

```bash
claude mcp add qt-commander -- qt-commander-mcp          # pip install
claude mcp add qt-commander -- uv run --project /absolute/path/to/qt-commander qt-commander-mcp   # source install
```

Claude Desktop / Cursor / other JSON clients:

```json
{
  "mcpServers": {
    "qt-commander": {
      "command": "qt-commander-mcp"
    }
  }
}
```

For a source checkout use `"command": "uv"` with
`"args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]`.
Use a native absolute path (`D:/Develop/qt-commander` is fine on Windows).
Restart the client after changing the config; `qt_status` should then list
descriptors or live sessions.

### 3. Connect a target

- **Linked runtime:** build/instrument the application or test app once (see
  [Build the linked runtime](#build-the-linked-runtime)); it writes a
  descriptor.
- **Already-running target:** call
  `qt_attach(pid=<pid>, mode="injection")`; use `qt_processes` to discover a
  PID, then `qt_target_info` when the target's Qt version/toolchain is unknown.
  Build the matching injector and runtime if needed — normally
  `qt_build(for_pid=<pid>)`, which derives every argument from the target and
  reports `restart_required`/`next_step`.

The usual first agent interaction is:

```text
qt_status -> qt_attach -> qt_windows -> qt_query -> qt_act
```

## Status

- Qt 5.15.2 verified with MinGW/MSVC; Qt 6.8.3 verified with both MinGW and
  MSVC for linked and injection. Ubuntu 24.04 + Qt 5.15.2 gcc_64 (XCB)
  verified for linked, the 99-test Python suite, and MCP stdio E2E.
- Linked mode: the application calls `qt_commander::start()` after
  `QApplication` and writes a rendezvous descriptor.
- Native injection (Windows and Linux): `qt_attach(pid=..., mode="injection")`
  invokes the C++ `qtc-inject` executable, which loads
  `qt_commander_inject.dll/.so`; the target-side library starts the same
  in-process server. `auto` injects when no live descriptor exists.
  Python is only the MCP orchestration layer; the target does not need Python
  or gdb. Linux ptrace still needs Yama permission (run the injector as the
  target's parent, or set `kernel.yama.ptrace_scope=0`). The native binary
  can be overridden with `QT_COMMANDER_INJECTOR`.
- 21 MCP tools are registered. Working today:
  `status`, `target_info`, `processes`, `environments`, `build`, `build_status`,
  `attach`, `detach`, `windows`, `query`, `items`, `act`, `batch`, `wait`,
  `screenshot`, `get`, `set`, `call`, `methods`, `source`, `task`. `source`
  searches the target's source tree (and `QT_COMMANDER_SOURCE_DIR` / an
  explicit `source_dir`) for matching `.ui` or `.qml`; unresolved targets
  return identity with `E_NO_DEBUGINFO`.
- Every data request is executed from a per-server FIFO queue driven by the
  target's event loop, so a slot blocked in `exec()` no longer holds the
  socket: reads, acts and `qt_task` keep working on the same connection.
  `qt_act`/`qt_call`/`qt_set` take `wait_ms` (defaults 1000/3000/3000 ms);
  when it expires they return `{"state": "running", "task_id": ...}` and the
  result is stored for `qt_task(task_id, wait_ms=...)`. Such a call has
  already been dispatched — never retry it. `qt_ping` reports
  `queued`/`inflight`/`active` for diagnostics.
- `items` covers `QTabWidget`/`QTabBar`, `QMenuBar`/`QMenu`, `QHeaderView`,
  `QTableView`, `QTreeView`, `QListView`, `QComboBox`, `QScrollBar`,
  `QSlider`, `QSpinBox`, checkable `QGroupBox`, `QDockWidget` title buttons,
  `QSplitter` handles and `QToolBar` actions; custom/proxy models work through
  the item-view adapter.
- QML deep objects under `QQuickWidget` are returned by `query` with
  `qml_type`/`qml_id`/`objectName`/local rect; use their `vqml` IDs with
  `qt_get`, `qt_act`, `qt_screenshot` and `qt_source`.
- QML-only apps are first-class too: `QQuickWindow` roots work in `qt_windows`,
  `qt_query`, `qt_items`, `qt_get`, `qt_set`, `qt_act` (QML click/key/type)
  and `qt_screenshot` (window or element crop).
- `qt_query` `limit` is a soft truncation: partial tree plus
  `count/total/limit/truncated` instead of a hard error.
- `qt_call` invokes only Qt metaobject methods (`slots` / `Q_INVOKABLE`) on
  widgets and QML items; property setters such as `QPushButton::setText` are
  not methods — use `qt_set` instead. Non-void return values are returned in
  `result`; object returns use `{class, id}` so they can be reused as targets.
  `qt_methods` lists invokable methods, parameter types and return types
  (Qt `_q_` internals are hidden unless `include_internal=true`).
  Pass `signature` to disambiguate overloads; enum arguments accept their
  integer values.
- `qt_target_info` inspects a process's PE/ELF architecture, full Qt runtime
  version (e.g. 5.15.2), build type, and toolchain before `qt_build`; it does
  not inject or launch. `detected_from` says which evidence won (`module` — the
  Qt runtime actually mapped into the process, authoritative — `adjacent`,
  `loader_path` or `descriptor`). It also reports `loaded_injector` (path,
  artifact, build_id, stale), `expected_artifact` (what `qt_attach` would
  inject), `injection_state` (`none`/`loaded`/`stale`/`mismatch`) and
  `restart_required`. `qt_processes` lists discoverable running Qt targets
  with their attach hint, without injecting.
- `qt_source` auto-discovers nearby source trees (including `src` ancestors)
  and accepts `source_dir` for qrc/embedded QML whose source tree is not next
  to the executable; it returns `file/line/url` when found.
- `qt_screenshot(annotate=...)` supports `annotate_offset` for paging large
  annotation sets; legend `n` numbers stay continuous across pages.
- `qt_batch` steps are exactly one of `{"act": ...}`, `{"wait": ...}`,
  `{"set": ...}`, `{"get": ...}`; this allows an ordered write/read/restore
  sequence (no implicit rollback).
- `qt_act(target=<window id>, action="minimize"|"restore")` controls a
  QWindow/QQuickWindow; restore preserves the pre-minimize geometry.
  `action="set_geometry"` with `geometry={x,y,w,h}` reports the actual
  `resolved.rect` and an `applied` flag.
- `qt_act` supports click/dblclick/type/focus/key/scroll/hover/drag and
  declarative `expect` (where/props/gone). `type` finds the first editable
  child for container targets and returns `E_NOT_EDITABLE` when none exists.
  Virtual items support click/dblclick.
- Targets may use a plain string as a QML `qml_id`, or combine
  `qml_id`/`text`/`class`/`objectName` (child text is included). Text matches
  fold into the nearest actionable ancestor; e.g. a label inside a button
  clicks the button. Hidden targets are excluded by default and return
  `E_HIDDEN` unless `include_hidden=true` is passed.
- Screenshots are capped at about 6 MiB at the MCP layer; oversized images are
  automatically re-captured at a smaller width (`auto_downscaled`).

## Build the linked runtime

```bash
cmake -S tests/test-apps/linked -B build/linked -G Ninja \
  -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5 \
  -DCMAKE_CXX_COMPILER=/path/to/mingw/g++.exe
cmake --build build/linked
```

MSVC users can instead call the MCP `qt_build` tool; it discovers Visual
Studio via `vswhere` and loads `vcvars64.bat` automatically (or accepts
`vcvars_path`).

On Linux with an official Qt kit, if Qt5GuiConfigExtras complains that
`GL/gl.h` is missing under `/usr/include/libdrm`, add
`-D_qt5gui_OPENGL_INCLUDE_DIR=/usr/include` to the configure command.

On Windows, put the Qt `bin` directory and the MinGW runtime on `PATH`, and
point `QT_PLUGIN_PATH` at the Qt `plugins` directory. Then run:

```powershell
$env:QT_COMMANDER_RENDEZVOUS = "$PWD\.test-workspace\rendezvous"
.\build\linked\qt-linked-test.exe
```

The app writes `descriptor-<pid>.json` into that directory (one file per
process; owner-only permissions on POSIX).

## Injection artifacts

`qt_build` writes injectors and injection libraries into one root only:

```text
artifacts/<os>-<arch>/<compiler>-<version>/qt-<qt-version>-<debug|release>/
  manifest.json
  bin/qtc-inject.exe
  lib/qt_commander_inject.dll   # libqt_commander_inject.so on Linux
```

Examples:

```text
artifacts/windows-x64/msvc-19.29/qt-5.15.2-debug/
artifacts/windows-x64/mingw-gcc-13.1/qt-5.15.2-release/
artifacts/linux-x64/gcc-13.3/qt-6.8.3-release/
```

`manifest.json` records `build_id` in the canonical
`qt<version>/<compiler>-<arch>/<build_type>` form (for example
`qt5.15.2/msvc-x64/Release`), the same string `qt_build` returns. The runtime's
descriptor carries the compiled-in id instead (`qt5/MSVC-AMD64/Release`), so do
not compare the two as plain strings; use
`qt_target_info.loaded_injector.build_id` against `expected_artifact.build_id`.

When the artifact was built from a git checkout, the manifest also carries a
`source` block with the release `version` and the `commit` it came from;
`qt_target_info` and `qt-commander doctor --pid` print it.

`qt_attach(mode="injection")` matches the target's OS/architecture,
compiler family and version, Qt version, and Qt debug/release against a
manifest under `artifacts/`; it does not scan `build/` or pick the newest file.
Set `QT_COMMANDER_INJECTOR` / `QT_COMMANDER_INJECT_DLL` only for an explicit
override.

Artifacts come from one of three places, in order of convenience:

1. `qt-commander install-artifacts` — downloads
   `qt-commander-artifacts-<os>-<arch>-*.zip` from the latest GitHub release,
   verifies `SHA256SUMS`, and unpacks it. `--from-dir`/`--from-url` cover
   offline machines, `--list` shows what a release carries.
2. `qt_build(for_pid=<pid>)` from an agent or MCP client — compiles against the
   Qt kit of a live target.
3. `scripts/package_artifacts.py` for a build you made yourself with CMake.

The artifact root is `artifacts/` in a source checkout. For an installed wheel
it is read-only, so `install-artifacts` falls back to a per-user directory; set
`QT_COMMANDER_ARTIFACTS` to pin one location explicitly (the MCP server and the
CLI both honour it).

The Qt debug/release flavour comes from evidence, never from a guess: the Qt
runtime actually mapped into the process wins, then the executable's own
directory, then the loader `PATH`, then the descriptor. A directory holding
both `Qt5Core.dll` and `Qt5Cored.dll` (a full Qt kit `bin` does) reports
`debug: null` with `flavor_ambiguous: true`, and artifact selection then asks
for an explicit `artifact_dir` instead of silently picking release. Picking the
wrong flavour injects a DLL that links the other Qt runtime, loads a second Qt
into the target, and times out waiting for `QCoreApplication` — which is why
`qt_build` refuses to default an unknown `build_type`.

## Run the MCP server

```bash
uv sync
uv run --project . qt-commander-mcp
```

If Windows reports `qt-commander-mcp.exe` is in use, stop the running
Claude/MCP process first, then rerun `uv run`.

Point the MCP client at the descriptor with `qt_attach(descriptor=...)`, or
set `QT_COMMANDER_RENDEZVOUS` before starting the server. For an already-running
Windows Qt app, use `qt_attach(pid=<pid>, mode="injection")`; the DLL path can
be overridden with `QT_COMMANDER_INJECT_DLL`.

The MCP server carries an `instructions` block for agent startup, target
selection, ambiguity/hidden handling, and destructive-action safety; each
tool also has a schema-aware description.

## Known limitations

- Qt Quick Controls `Popup` roots (for example `indicatorPopup`) may not be
  discoverable by `qml_id`; query their text or child items instead.
  A `Popup` rendered in the main window's Overlay is not a separate window, so
  `qt_windows` still reports only the main window. Independent `Qt.Popup`
  windows do appear and have `popup: true`.
- After changing C++ runtime code, rebuilding the artifact and restarting the
  target is required. Restarting only the MCP server leaves the already-loaded
  runtime in the old target process. A library already mapped into a process
  cannot be replaced by injecting again: `LoadLibraryW` on the same path only
  increments its reference count, so `DllMain` never runs. `qt_target_info`
  reports this as `loaded_injector.loaded` with `restart_required: true`, and
  `qt_attach` refuses with `reason="injector_already_loaded"` instead of
  silently doing nothing.
- The injector's Qt debug/release flavour must match the flavour of the Qt
  runtime the target loaded (`Qt5Core.dll` vs `Qt5Cored.dll`). See
  [Injection artifacts](#injection-artifacts).

## Troubleshooting

- Start with `qt-commander doctor --json` (add `--pid <pid>` to inspect one
  target). It reports Python and dependencies, Qt kits, toolchains, installed
  artifacts and live descriptors, plus the artifact `qt_attach` would pick for
  that target and whether it needs a restart.
- `qt_status` has no descriptors: start a linked target, or use
  `qt_processes` and then `qt_attach(pid=..., mode="injection")`.
- A desktop client cannot find `uv`: use the absolute path to `uv` as
  `command` in the client config.
- `qt-commander-mcp.exe` is locked: stop the old Claude/MCP process, then
  restart the client.
- Injection fails or attaches to an incompatible runtime: read
  `error.reason` first. `injector_already_loaded` and
  `qt_runtime_flavor_mismatch` mean the target must be restarted;
  `artifact_missing` means run `qt-commander install-artifacts` or
  `qt_build(for_pid=<pid>)`. `error.details`
  carries the target snapshot, the artifact that was picked, the injector
  already loaded, and the artifact `qt_attach` expected. Do not bypass a
  flavour mismatch by forcing `artifact_dir` — the library will not start.

For multiple windows, call `qt_windows` first and pass that window's `id` to
`qt_screenshot`. Without a target, a screenshot is refused when more than one
visible top-level window exists; popups/tooltips do not count toward that
ambiguity. `qt_query(scope=...)` can limit a query to one window/widget subtree;
without `props`, nodes default to `text/visible/enabled`, which is usually
enough to identify a target. `where` accepts both `object_name` and the `objectName` alias, and strings
support wildcards or `contains/startsWith/regex/equals` matchers. For QAction/
menu items use `qt_items(owner=menu, where=...)`; they are virtual items, not
part of the QWidget query tree.

## Test

```bash
uv run pytest -q          # 152 tests, no Qt installation required
```

The pure-QML regression app is built with the same options as the linked app:

```bash
cmake -S tests/test-apps/qmlwindow -B build/qmlwindow-mingw5 -G Ninja \
  -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5
cmake --build build/qmlwindow-mingw5
```

End-to-end / benchmark scripts:

```bash
# linked smoke (start tests/test-apps/linked first)
uv run python scripts/linked_smoke.py
uv run python scripts/mcp_linked_smoke.py
uv run python scripts/window_regression.py
uv run python scripts/qml_window_test.py

# native injection smoke (pass the target pid)
uv run python scripts/injection_smoke.py <pid> --mode injection

# async task queue: pending -> task_id -> qt_task, nested exec, batch, soak
cmake -S tests/test-apps/modal -B build/modal-mingw5 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DQT_MAJOR_VERSION=5 \
  -DQt5_DIR=/path/to/Qt/5.15.2/mingw81_64/lib/cmake/Qt5
cmake --build build/modal-mingw5
uv run python scripts/task_regression.py

# P0 benchmarks
uv run python scripts/p0_large.py --label large-7000
uv run python scripts/p0_matrix.py --exe build/matrix-mingw5/qt-matrix-test.exe
```

## Layout

```
qt_commander/        Python MCP server, protocol, client, session, ops, injector adapter
qt_commander/cli.py  qt-commander serve / doctor / install-artifacts
src/                 C++ runtime (linked library + injection DLL)
src/injector/        native C++ qtc-inject (Windows remote thread / Linux ptrace)
tests/unit_server/   Python unit tests
tests/test-apps/     linked / large / injected / matrix / qmlwindow test applications
scripts/             smoke, injection, benchmark and release packaging scripts
docs/design.md       current design notes
docs/agent-eval.md   agent acceptance tasks and metrics
docs/release/        release checklist and licence decision notes
docs/benchmarks/     large-7000.json and matrix.json P0 results
ROADMAP.md           what comes next, and what is explicitly out of scope
CHANGELOG.md         release history
SECURITY.md          injection threat model, hardening, disclosure
CONTRIBUTING.md      setup, repository map, how to add a tool
CODE_STANDARDS.md    coding and review rules
```

## Project

- [ROADMAP.md](ROADMAP.md) — priorities and non-goals
- [CHANGELOG.md](CHANGELOG.md) — what changed, per release
- [CONTRIBUTING.md](CONTRIBUTING.md) — build, test, and how a tool is wired
- [SECURITY.md](SECURITY.md) — read this before injecting into anything you
  care about

## License

MIT. See [LICENSE](LICENSE).

Earlier releases (`v0.8.0` and before) were published under
`MIT AND Anti-996`; they keep the terms they were published with. The reasoning
behind the change is recorded in
[docs/release/LICENSE-NOTES.md](docs/release/LICENSE-NOTES.md), and the old
anti-996 text is kept as a non-binding note in
[docs/community/anti-996.md](docs/community/anti-996.md).
