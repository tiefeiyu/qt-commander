# qt-commander

Live Qt UI introspection and automation for AI agents.

qt-commander links a small in-process runtime into a Qt application, exposes
its live object tree over a local JSON-RPC connection, and drives it through
an MCP server. Queries are evaluated in the target process every time; there
are no snapshot files and no stale index of the UI.

## Status

- Qt 5.15.2 verified with MinGW/MSVC; Qt 6.8.3 verified with both MinGW and
  MSVC for linked and injection. Ubuntu 24.04 + Qt 5.15.2 gcc_64 (XCB)
  verified for linked, the 72-test Python suite, and MCP stdio E2E.
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
- 16 MCP tools are registered. Working today:
  `status`, `environments`, `build`, `attach`, `detach`, `windows`, `query`,
  `items`, `act`, `batch`, `wait`, `screenshot`, `get`, `set`, `call`, `source`.
  `source` uses `.ui`/`.qml`-by-objectName heuristics and otherwise returns
  identity with `E_NO_DEBUGINFO`.
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
- `qt_source` accepts `source_dir` for qrc/embedded QML whose source tree is
  not next to the executable; it returns `file/line/url` when found.
- `qt_screenshot(annotate=...)` supports `annotate_offset` for paging large
  annotation sets; legend `n` numbers stay continuous across pages.
- `qt_batch` steps are exactly one of `{"act": ...}`, `{"wait": ...}`,
  `{"set": ...}`, `{"get": ...}`; this allows an atomic write/read/restore
  sequence.
- `qt_act(target=<window id>, action="minimize"|"restore")` controls a
  QWindow/QQuickWindow; restore preserves the pre-minimize geometry.
- `qt_act` supports click/dblclick/type/focus/key/scroll/hover/drag and
  declarative `expect` (where/props/gone). `type` finds the first editable
  child for container targets and returns `E_NOT_EDITABLE` when none exists.
  Virtual items support click/dblclick.
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
uv run pytest tests/unit_server -q
```

Current suite: **72 passed** (Qt5 MinGW/ MSVC test apps are built separately).

End-to-end / benchmark scripts:

```bash
# linked smoke (start tests/test-apps/linked first)
uv run python scripts/linked_smoke.py
uv run python scripts/mcp_linked_smoke.py
uv run python scripts/window_regression.py
uv run python scripts/qml_window_test.py

# native injection smoke (pass the target pid)
uv run python scripts/injection_smoke.py <pid> --mode injection

# P0 benchmarks
uv run python scripts/p0_large.py --label large-7000
uv run python scripts/p0_matrix.py --exe build/matrix-mingw5/qt-matrix-test.exe
```

## Layout

```
qt_commander/        Python MCP server, protocol, client, session, ops, injector adapter
src/                 C++ runtime (linked library + injection DLL)
src/injector/        native C++ qtc-inject (Windows remote thread / Linux ptrace)
tests/unit_server/   Python unit tests
tests/test-apps/     linked / large / injected / matrix test applications
scripts/             smoke, injection and P0 benchmark scripts
docs/design.md       current design notes
docs/benchmarks/     large-7000.json and matrix.json P0 results
CODE_STANDARDS.md    coding and review rules
```

## License

MIT AND Anti 996. See `LICENSE`, `LICENSE.MIT`, `LICENSE.996ICU`.
