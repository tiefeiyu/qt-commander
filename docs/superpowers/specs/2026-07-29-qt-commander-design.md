# qt-commander Design Specification

**Date:** 2026-07-29
**Status:** Review (v3 — post 12-agent audit)
**Target:** Cross-platform MCP server for Qt application introspection and automation

---

## 1. Overview

qt-commander is an MCP (Model Context Protocol) server that enables AI agents to inspect and automate
Qt applications at runtime — analogous to Playwright for the browser, but targeting native Qt widgets
and QML scenes.

### 1.1 Core Capabilities

- Build an injection library compatible with a specific Qt toolchain
- Inject that library into a running Qt process
- Enumerate and traverse the complete UI element tree (Widgets + QML)
- Read and write arbitrary QObject properties via the Qt meta-object system
- Simulate mouse, keyboard, touch, focus, and context-menu events
- Capture screenshots of windows and individual elements
- Expose everything to an AI agent through MCP tools over stdio or HTTP

### 1.2 Non-Goals

- Operating on processes that are not Qt applications (dynamically linked Qt required)
- Injecting into hardened / sandboxed / SIP-protected / code-signed processes
- Wayland: window title extraction and screen-level grab are unsupported
- macOS Hardened Runtime or notarized applications
- Statically linked Qt applications (no Qt DLLs/SOs loaded means undetectable)
- Drag-and-drop automation (deferred — Qt's QDrag::exec() blocks, making fine-grained control impractical without re-entrant event loop management)
- Native gesture simulation (deferred — QNativeGestureEvent synthesis is under-specified by Qt)
- Remote injection across machines (same-machine only)
- GUI for the MCP server itself

### 1.3 Known Platform Limitations

| Limitation | Windows | Linux/X11 | Linux/Wayland | macOS |
|-----------|---------|-----------|---------------|-------|
| Injection into running processes | ✅ | ✅ (needs ptrace) | ✅ (needs ptrace) | ⚠️ SIP/Hardened Runtime restrict |
| Window title extraction | ✅ | ✅ (XGetWindowProperty) | ❌ (protocol prohibits it) | ✅ (CGWindowList, needs permission) |
| Screen-level screenshot | ✅ | ✅ | ❌ (returns empty pixmap) | ✅ |
| Widget-level screenshot | ✅ | ✅ | ✅ (QWidget::grab) | ✅ |
| QML item screenshot | ✅ | ✅ | ✅ | ✅ |
| Architecture matching required | Yes (32/64-bit must match) | Yes (x86_64/ARM64 must match) | Yes | Yes (x86_64/ARM64 must match) |

---

## 2. Architecture

### 2.1 Process Model

Two processes:

| Process | Role |
|---------|------|
| **qt-commander** | MCP server. Accepts stdio/HTTP connections from AI agents. Manages sessions, orchestrates builds, injects libraries. |
| **Qt Target App** | The Qt application being automated. `libqt-commander` is injected into its address space. |

### 2.2 Communication Channels

| Channel | Carrier | Content |
|---------|---------|---------|
| **AI ↔ MCP Server** | stdio or HTTP/SSE (MCP protocol) | Tool calls, MCP resources |
| **MCP Server ↔ Injected Library** | TCP loopback (127.0.0.1) + custom framed protocol | Commands, small results, errors |
| **Large data** | File system via MCP Resources | UI tree snapshots, screenshots, binary property values |

### 2.3 MCP Protocol Integration

The server advertises during `initialize`:
```json
{
  "capabilities": {
    "tools": {},
    "resources": {
      "subscribe": true,
      "listChanged": true
    },
    "logging": {}
  },
  "protocolVersion": "2024-11-05"
}
```

**Resource URI scheme:**
```
qt-commander://sessions/{session_id}/snapshots/{snapshot_id}
qt-commander://sessions/{session_id}/screenshots/{screenshot_id}
qt-commander://sessions/{session_id}/data/{blob_id}
```

Tool results return *only* lightweight metadata + resource URIs. Large data (UI trees, screenshots)
is fetched by the AI agent via `resources/read`. The server resolves resource URIs to filesystem
paths: e.g. `snapshots/5` → `00000005_ui_tree.json` (numeric URI segment → 8-digit zero-padded
filename). MCP `resources/list` responses include the full URI with numeric IDs. This replaces the
previous `$ref` pattern, which is not an MCP protocol feature.

```json
// Tool result example
{
  "result": {
    "session_id": "a3b9f2c8e1d4",
    "snapshot_id": 5,
    "element_count": 284,
    "uri": "qt-commander://sessions/abc/snapshots/5"
  }
}
```

### 2.4 TCP Framing Protocol

Raw JSON-RPC over TCP is unreliable without message boundaries. We use a **length-prefix frame**
with magic and version bytes for protocol evolution:

```
┌──────┬─────────┬──────────────┬──────────────────────────────┐
│ 1 B  │  1 B    │  4 bytes BE  │  N bytes                      │
│ magic│ version │ payload len  │  JSON-RPC 2.0 message (UTF-8) │
│ 0xCC │  0x01   │  (uint32_t)  │                               │
└──────┴─────────┴──────────────┴──────────────────────────────┘
```

- **Magic:** `0xCC`. Detecting this byte on the wire confirms we are looking at a qt-commander frame
  (as opposed to garbage data after a TCP error). If the first byte is not `0xCC`, the receiver
  resets the framing state machine and closes the connection (unrecoverable).
- **Version:** `0x01` for the initial protocol. Allows future frame format changes
  (e.g., adding checksums, stream multiplexing, different length widths) without breaking
  compatibility. Receivers reject unknown versions by closing the connection.
- **Length:** 4 bytes, unsigned big-endian (`uint32_t`). Valid range: `[1, 16777216]` (1 byte to 16 MB).
  Length=0 is a protocol error → close connection. Length > 16 MB is rejected before allocation
  (error response: `{"jsonrpc":"2.0","error":{"code":-32000,"message":"frame too large","data":{"code":2008,"max_bytes":16777216}},"id":null}`,
  then close).
- **Payload:** UTF-8 encoded JSON-RPC 2.0 request/response/notification.
- Receivers read exactly 6 bytes (header), validate magic+version, parse length, then read exactly N bytes.
- Partial `send()`/`recv()` results are looped until complete.
- Framing decoder accumulates bytes incrementally. If a read error occurs mid-frame, the connection
  is dropped and the accumulated buffer is discarded.

### 2.5 Channel Authentication

Every control-channel connection must authenticate with a shared session token:

1. MCP server generates a random 32-byte token (hex-encoded, 64 chars, from CSPRNG).
2. Token is written to the port handshake file (Section 9.4).
3. Library requires authentication as the **first framed JSON-RPC request** after TCP connect.
4. Server sends:
   ```json
   {"jsonrpc":"2.0","method":"qt.authenticate","params":{"token":"<64-char-hex>"},"id":1}
   ```
5. Library validates the token. On success:
   ```json
   {"jsonrpc":"2.0","result":{"session_id":"abc"},"id":1}
   ```
6. On failure (wrong token or non-auth first message): library sends
   ```json
   {"jsonrpc":"2.0","error":{"code":-32000,"message":"authentication failed","data":{"code":2009}},"id":<original_id>}
   ```
   then closes the TCP connection.
7. If no auth message is received within 5 seconds, library closes the connection without a response.

**Port file permissions:** The port file is created with restrictive permissions
(`0600` on POSIX, `FILE_ATTRIBUTE_HIDDEN` + owner-only DACL on Windows). After successful
authentication, the library deletes the port file.

---

## 3. Workspace Layout

```
<workspace>/                          # Default: ~/.qt-commander or %APPDATA%/qt-commander
├── builds/                           # Build artifacts
│   └── <qt_ver>_<toolchain>_<arch>/  # e.g. qt5.15_msvc2019_x64
│       ├── src/                      # Snapshot of library source at build time
│       ├── build/                    # CMake build directory
│       └── output/
│           └── libqt-commander.{dll|so|dylib}
│
├── sessions/                         # Per-session runtime data
│   └── <session_id>/                 # One directory per attached Qt process
│       ├── port.txt                  # TCP port + auth token
│       ├── snapshots/
│       │   ├── 00000001_ui_tree.json
│       │   └── 00000002_ui_tree.json
│       ├── screenshots/
│       │   ├── 00000001_full.png
│       │   └── 00000002_element_42.png
│       └── data/                     # Large property values, binary blobs
│
└── logs/
    └── qt-commander.log
```

**Principles:**
- One workspace, everything inside — no scattered temp files.
- Workspace path configurable via CLI flag `--workspace` or config file.
- Sessions are isolated; `qt_detach` can optionally purge the session directory.
- Server startup scans `sessions/` for orphaned directories (no live session) and logs warnings.
- `port.txt` is listed in the session directory (not omitted from the layout).

---

## 4. Dependency Strategy

### 4.1 MCP Server (qt-commander executable)

| Dependency | Reason |
|------------|--------|
| C++17 standard library | Base requirement |
| [nlohmann/json](https://github.com/nlohmann/json) (single-header v3.x) | JSON-RPC parsing. |
| Platform sockets (WinSock / POSIX) | TCP loopback to injected library |
| Platform process API | Injection (OpenProcess, ptrace, Mach) |

The server does NOT depend on Qt. HTTP/SSE transport is implemented on raw sockets:
HTTP 1.1 parsing is conceptually simple over a framed reader (read headers until `\r\n\r\n`,
then read Content-Length body). SSE is equally straightforward (send `data:` frames and flush).

### 4.2 Injected Library (libqt-commander)

| Dependency | Reason |
|------------|--------|
| C++17 standard library | Base requirement |
| Qt 5 or Qt 6 (Core, Gui, Widgets, Quick) | Already loaded by host process |
| Qt's built-in QJsonDocument | JSON handling |

**Zero additional dependencies.** The library piggybacks on the Qt installation already loaded
in the target process.

---

## 5. MCP Tools

Each tool is registered with a formal MCP `inputSchema` (JSON Schema). The tables below use
an informal notation for readability, but every tool must have a complete `inputSchema`
with `required` arrays, typed `properties`, and a `description` field.

### 5.1 Session Management

| Tool | Required Params | Optional Params | Returns | Description |
|------|----------------|-----------------|---------|-------------|
| `qt_list_processes` | — | — | `[{pid, name, title, qt_version, arch, bitness}]` | List running Qt processes. `arch` is `"x86_64"`/`"arm64"`/`"x86"`; `bitness` is 32 or 64. Window title extraction is best-effort. |
| `qt_attach` | `pid: integer` | — | `{session_id}` | Inject library into process, perform port handshake, establish TCP connection. Fails if PID already attached. |
| `qt_detach` | `session_id: string` | `purge: boolean` | `{ok, purged_files?}` | Send shutdown RPC, close TCP, optionally eject library, optionally purge session directory. |
| `qt_list_sessions` | — | — | `[{session_id, pid, connected, snapshot_count}]` | List active sessions. |

### 5.2 UI Inspection

| Tool | Required Params | Optional Params | Returns | Description |
|------|----------------|---------------|---------|-------------|
| `qt_snapshot` | `session_id: string` | `include_hidden: boolean` (default false), `detail: string` (default `"extended"`) | `{session_id, snapshot_id, element_count, uri}` | Full UI tree as MCP Resource. Invalidates previous element IDs. `detail`: `"core"` \| `"extended"` \| `"full"`. |
| `qt_find_element` | `session_id: string`, `query: object` | — | `[{element_id, type, text, rect, properties (core tier)}]` | Find elements matching a JSON query. Returns a subset of snapshot fields sufficient for identification. |
| `qt_get_property` | `session_id: string`, `element_id: integer`, `name: string` | — | `{value, uri?}` | Read a single property. Returns `value` inline or `uri` for large binary properties. Shape depends on type — see Section 8.3. |
| `qt_set_property` | `session_id: string`, `element_id: integer`, `name: string`, `value` | — | `{ok}` | Write a property value. |
| `qt_call_method` | `session_id: string`, `element_id: integer`, `method: string` | `args: array` | `return_value` | Invoke a QObject slot/method via QMetaObject::invokeMethod. |
| `qt_screenshot` | `session_id: string` | `element_id: integer` | `{uri, width, height}` | Screenshot of entire window (no element_id) or specific element. Saved as MCP Resource. |

### 5.3 Input — Mouse

All mouse tools use coordinates **relative to the element's local coordinate system** (widget content area for QWidget, local coordinates for QQuickItem). Coordinates default to the element center if omitted.

| Tool | Required | Optional | Notes |
|------|----------|----------|-------|
| `qt_mouse_press` | `session_id`, `element_id` | `button` (default "left"), `x`, `y`, `modifiers` | QMouseEvent::MouseButtonPress |
| `qt_mouse_release` | `session_id`, `element_id` | `button`, `x`, `y`, `modifiers` | QMouseEvent::MouseButtonRelease |
| `qt_mouse_click` | `session_id`, `element_id` | `button`, `x`, `y`, `modifiers` | Convenience: press + release |
| `qt_mouse_dblclick` | `session_id`, `element_id` | `button`, `x`, `y`, `modifiers` | QMouseEvent::MouseButtonDblClick |
| `qt_mouse_move` | `session_id`, `element_id`, `x`, `y` | — | QMouseEvent::MouseMove |
| `qt_mouse_wheel` | `session_id`, `element_id` | `delta_x`, `delta_y`, `x`, `y`, `pixel_delta` | QWheelEvent. For QQuickItem, uses QQuickWindow sendEvent path. |

**button values:** `"left"`, `"right"`, `"middle"`, `"back"`, `"forward"`
**modifiers:** `["shift", "ctrl", "alt", "meta", "keypad"]`

### 5.4 Input — Keyboard

| Tool | Required | Optional | Notes |
|------|----------|----------|-------|
| `qt_key_press` | `session_id`, `key` | `modifiers`, `text` | QKeyEvent::KeyPress. Target is the currently focused widget. |
| `qt_key_release` | `session_id`, `key` | `modifiers`, `text` | QKeyEvent::KeyRelease |
| `qt_type_text` | `session_id`, `text` | `interval_ms` | Convenience: character-by-character key presses. Delivers to focused widget. |
| `qt_key_combo` | `session_id`, `keys` | — | Parse "Ctrl+C", "Alt+F4" into press+release sequence. |

**key values:** Qt::Key enum names — e.g. `"Key_A"`, `"Key_Return"`, `"Key_Escape"`, `"Key_F1"`,
`"Key_Tab"`, `"Key_Space"`, `"Key_Left"`, `"Key_Up"`, `"Key_Home"`, `"Key_PageUp"`.
Parsed via `QMetaEnum::fromType<Qt::Key>().keyToValue()`.

Note: `qt_type` was renamed to `qt_type_text` to avoid confusion with Qt's "type" terminology
(QEvent::Type, QMetaType, className, QML typeof).

### 5.5 Input — Touch

| Tool | Required | Optional | Notes |
|------|----------|----------|-------|
| `qt_touch_press` | `session_id`, `element_id`, `x`, `y` | `touch_id`, `pressure` | QTouchEvent::TouchBegin. Requires registered QTouchDevice (created once on first touch call). |
| `qt_touch_move` | `session_id`, `element_id`, `x`, `y`, `touch_id` | `pressure` | QTouchEvent::TouchUpdate |
| `qt_touch_release` | `session_id`, `touch_id` | `element_id`, `x`, `y` | QTouchEvent::TouchEnd |

A `QTouchDevice` of type `TouchScreen` with `Position` capability is automatically registered
on the library's first touch operation.

### 5.6 Input — Focus & Other

| Tool | Required | Optional | Notes |
|------|----------|----------|-------|
| `qt_focus` | `session_id`, `element_id` | `reason` | Calls `QWidget::setFocus()` / `QQuickItem::forceActiveFocus()`. **Not** raw QFocusEvent — uses proper Qt focus chain APIs. |
| `qt_clear_focus` | `session_id`, `element_id` | — | Calls `QWidget::clearFocus()`. |
| `qt_context_menu` | `session_id`, `element_id` | `x`, `y` | QContextMenuEvent at element-local coordinates. |

**Activation:** There is no universal `qt_activate` tool. Different Qt widget types have different
activation mechanisms (QAbstractButton::click(), QCheckBox::toggle(), QComboBox::showPopup()).
The AI agent should use `qt_call_method` or `qt_mouse_click` for activation, or `qt_set_property`
to set `checked`/`currentIndex` properties.

### 5.7 Build

| Tool | Required | Optional | Returns | Description |
|------|----------|----------|---------|-------------|
| `qt_build_library` | `qt_env: string` | `vcvars: string`, `vcvars_args: string`, `arch: string`, `generator: string`, `qt_major_version: integer` (default 5) | `{library_path, qt_version, arch, build_key}` | Compile injection library using target Qt toolchain. `vcvars` is required on Windows, ignored on other platforms. `generator` auto-detected (jom → ninja → nmake make) but can be overridden. |

---

## 6. Element Selector Design

### 6.1 Primary Path: Element ID from Snapshot

The primary workflow does not use selectors at all:

```
qt_snapshot() → reads UI tree JSON → AI identifies element id=42 → qt_click(element_id=42)
```

`qt_find_element` is a fallback for when the UI has changed since the last snapshot.

### 6.2 Selector Format

Selectors are JSON objects (not strings) because AI agents generate JSON naturally from
snapshot data, and the format is type-safe.

```json
{
  "type": "QPushButton",
  "type_inherits": "QAbstractButton",
  "text": "OK",
  "text_contains": "Sub",
  "object_name": "btnSubmit",
  "window_title": "Settings",
  "window_title_contains": "Sett",
  "properties": {
    "visible": true,
    "enabled": true,
    "placeholderText": "Email"
  },
  "ancestor_id": 3,
  "window_id": 1,
  "depth": "deep",
  "limit": 1
}
```

| Field | Type | Semantics |
|-------|------|-----------|
| `type` | string | Exact match on `QMetaObject::className()`. Uses C++ class name (e.g. `QPushButton`), NOT QML type name. |
| `type_inherits` | string | IS-A check — walks `QMetaObject::superClass()` chain. Matches if any ancestor class name contains this string. Replaces the ambiguous `type_contains`. |
| `text` | string | Display text exact match. For windows: windowTitle. For buttons/labels: text(). For QQuickItem: text property if available. |
| `text_contains` | string | Display text substring match (case-sensitive). |
| `object_name` | string | Exact match on `QObject::objectName()`. |
| `window_title` | string | Containing window title exact match. |
| `window_title_contains` | string | Window title substring match. |
| `properties` | object | Arbitrary Qt property filters. Values are matched using `QVariant::operator==` after deserialization. |
| `ancestor_id` | integer | Limit search to descendants of a specific element. |
| `window_id` | integer | Limit search to a specific top-level window (the window_id from snapshot). |
| `depth` | string | `"exact"` = direct children only, `"shallow"` = ≤2 levels, `"deep"` = recursive (default). When `ancestor_id` is set, depth counts from that ancestor. |
| `limit` | integer | Max results. Default 0 (unlimited). |

All fields are optional and AND-combined.

### 6.3 Snapshot Element Reference Fields

To support queries that need spatial awareness, each element in the snapshot includes:

```json
{
  "id": 42,
  "type": "QPushButton",
  "text": "OK",
  "rect": {"x": 300, "y": 500, "width": 80, "height": 24},
  "global_rect": {"x": 600, "y": 700, "width": 80, "height": 24},
  "z_order": 5,
  "parent_id": 3,
  "object_parent_id": 4,
  "window_id": 1,
  "window_title": "My Application",
  "child_indices": [43, 44],
  "properties": { ... }
}
```

- `rect`: element-local coordinates (relative to parent's content area for QWidget, local coords for QQuickItem)
- `global_rect`: screen-absolute coordinates
- `z_order`: stacking order (0 = bottom). Higher values are visually on top.
- `window_id`: top-level window this element belongs to
- `child_indices`: ordered list of child element IDs (visual order)

### 6.4 Property Value Matching

Property filter values are deserialized to `QVariant` and compared using `QVariant::operator==`.
For number types, the values are coerced to the same numeric type before comparison.
Enums are serialized as integer values; string-based enum matching is not supported
(snapshots include the integer, AI should match on the integer).

### 6.5 Snapshot Size Management

To avoid multi-megabyte snapshots, property serialization uses a **tiered approach**:

| Tier | Included | Approx. Element Size |
|------|----------|---------------------|
| **Core** (always) | type, text, rect, global_rect, visible, enabled, objectName, windowTitle, z_order, parent_id, window_id, child_indices | ~500 bytes |
| **Extended** (default) | Core + all Q_PROPERTY values via `QMetaObject` iteration + dynamic properties via `QObject::dynamicPropertyNames()`. All properties are included regardless of whether they differ from the default — the "non-default" filter is not practically implementable since Qt's `QMetaProperty::defaultValue()` is optional and rarely populated. Binary/callback/QObject* properties replaced with markers. | ~2-5 KB |
| **Full** | Extended + binary properties resolved to data/ URIs (screenshots of icons, pixmaps, cursors). Includes full recursive serialization of complex types. | 10-100+ KB |

`qt_snapshot` accepts a `detail: string` parameter: `"core"`, `"extended"` (default), or `"full"`.
The AI agent can use `qt_get_property` to fetch individual properties on demand.

**Design rationale:** `QMetaProperty::defaultValue()` is optional in Q_PROPERTY declarations and
rarely populated outside Qt's own classes. A fallback of constructing a fresh instance and comparing
all properties is prohibitively expensive (side effects, no guarantee of no-arg constructors).
Tier "extended" accepts the size cost of including all properties (~2-5 KB per widget for a
QPushButton) as acceptable for local-machine tooling. Tier "core" provides the lightweight option.

---

## 7. Element ID Lifecycle

### 7.1 Mapping Table (Thread-Safe)

```cpp
// Protected by QReadWriteLock
mutable QReadWriteLock map_lock_;
QHash<quint64, QObject*> element_map_;  // raw pointer, NOT QPointer
quint64 next_id_ = 1;
quint64 current_epoch_ = 0;
```

**Key design changes from v1 (based on audit findings):**
- `QPointer` is **not thread-safe** (confirmed by Qt docs and implementation). It can auto-null
  while a worker thread reads it, modifying the QHash value memory mid-read.
- We use raw `QObject*` with a `QReadWriteLock`: main thread takes write lock during snapshot
  clear/rebuild; worker thread takes read lock during lookups.
- An **epoch counter** increments on each snapshot. The worker thread caches the epoch at lookup
  time and validates it hasn't changed after dispatching to main thread.

### 7.2 Snapshot Semantics

Each `qt_snapshot` call:

1. Acquires write lock on `map_lock_`
2. Clears the previous `element_map_` — all old IDs become invalid
3. Increments `current_epoch_`
4. Walks the UI tree iteratively (explicit `QStack`, not recursion, to avoid stack overflow)
   - QWidget tree: `QApplication::allWidgets()` filtered by `isVisible()` and `isWindow()`
   - QQuickItem tree: from `QQuickWindow::contentItem()` via `childItems()`
   - Mixed: `QQuickWidget` detected and its QQuickWindow contentItems traversed separately
   - Cycle detection: `QSet<QObject*> visited`; skip already-visited objects
   - Max depth guard: 1000 levels
5. Assigns sequential IDs to each element
6. Writes `element_map_` entries: `{1 → w, 2 → btn, ...}` (raw pointers)
7. Releases write lock
8. Serializes the tree as a flat JSON array (now includes `global_rect`, `z_order`, `window_id`, `child_indices`)
9. Writes JSON atomically: temp file → `rename()` (or `MoveFileEx` on Windows) → final path
10. Handles edge cases:
    - **Empty tree** (`element_count: 0`): valid snapshot with `"elements": []`, no error.
    - **Cycle detected**: snapshot completes with `"truncated": true, "truncated_reason": "cycle_detected"`.
      Elements visited before the cycle are included; the cycle entry point is skipped.
    - **Max depth reached**: `"truncated": true, "truncated_reason": "max_depth_1000"`.
      Elements at depth <= 1000 are included; deeper subtrees are omitted.
    - **QQuickWindow::contentItem() null** (window not initialized): window is skipped,
      not reported as error. AI can retry `qt_snapshot` later.
    - **QCoreApplication/QGuiApplication without QApplication**: widget traversal is skipped
      gracefully. If only QML windows exist, QML traversal proceeds normally.

Optional parameters:
- `include_hidden: boolean` (default false) — include non-visible elements
- `detail: string` (default `"extended"`) — property serialization tier

### 7.3 Error Handling

| Scenario | Detection | JSON-RPC Error (code) | Message |
|----------|-----------|----------------------|---------|
| Element destroyed | Raw pointer invalid after read-lock + epoch check | `-32000` data.code=1001 | "element #42 no longer exists" |
| ID from old snapshot | ID not in current `element_map_` | `-32000` data.code=1002 | "element #42 is stale — call qt_snapshot first" |
| Element not visible | Check `visible` property on main thread | `-32000` data.code=1003 | "element #42 is not visible" |
| Element disabled | Check `enabled` property on main thread | `-32000` data.code=1004 | "element #42 is disabled" |
| Invalid session_id | Not found in session manager | `-32602` | "Invalid params: unknown session_id" |
| Build failure | CMake or compiler error | `-32000` data.code=2001 | "Build failed: {details}" |
| Injection failure | OS API error | `-32000` data.code=2002 | "Injection failed: {details}" |
| RPC timeout | 30s no response from library | `-32000` data.code=2003 | "Target process did not respond" |
| Main thread unresponsive | `QSemaphore::tryAcquire` timeout | `-32000` data.code=2004 | "Main thread unresponsive after 30s" |
| Disk full | `std::filesystem::space` check fails | `-32000` data.code=2005 | "Disk full: {available} MB remaining, need {required} MB" |
| Library already attached | PID already has active session | `-32000` data.code=2006 | "Process {pid} already attached in session {session_id}" |
| Operation in progress | Atomic busy flag set | `-32000` data.code=2007 | "operation in progress — retry" |
| Frame exceeds max size | Length > 16 MB in frame header | `-32000` data.code=2008 | "frame too large (max 16 MB)" |
| Authentication failed | Wrong token or non-auth first message | `-32000` data.code=2009 | "authentication failed" |
| Element zero-size | width=0 or height=0, operation unsafe | `-32000` data.code=1005 | "element #42 has zero size" |
| Snapshot truncated | Cycle detected or max depth reached | `-32000` data.code=2010 | "snapshot truncated: cycle detected" or "max depth 1000 reached" |

### 7.4 Snapshot Numbering

Snapshots are numbered sequentially per session with 8-digit zero-padded format:
`00000001`, `00000002`, ... (supports up to 99,999,999 snapshots).

---

## 8. UI Tree Serialization

### 8.1 Flat List Format

The snapshot JSON is a flat list with parent references. This is easier to stream and parse
incrementally than a deeply nested tree, and naturally handles reparenting between snapshots.

```json
{
  "session_id": "a3b9f2c8e1d4",
  "snapshot_id": 5,
  "epoch": 5,
  "timestamp_ms": 1712188800000,
  "element_count": 284,
  "detail": "extended",
  "elements": [
    {
      "id": 1,
      "type": "QMainWindow",
      "text": "My Application",
      "rect": {"x": 0, "y": 0, "width": 800, "height": 600},
      "global_rect": {"x": 100, "y": 100, "width": 800, "height": 600},
      "z_order": 0,
      "parent_id": null,
      "object_parent_id": null,
      "window_id": 1,
      "window_title": "My Application",
      "child_indices": [2, 10, 15],
      "properties": {
        "visible": true,
        "enabled": true,
        "windowTitle": "My Application",
        "objectName": "MainWindow"
      }
    }
  ]
}
```

### 8.2 QML Dual-Parent

For QQuickItem elements, `parent_id` is the visual parent
(`QQuickItem::parentItem()`) and `object_parent_id` is the QObject parent
(`QObject::parent()`). The two differ when QML internal objects (Binding,
QQmlComponent, etc.) are QObject parents but not visual parents.

### 8.3 Property Serialization Rules

| Qt Type | JSON Serialization |
|---------|-------------------|
| `bool` | JSON boolean |
| `int`, `uint`, `qint64`, `quint64` | JSON number |
| `float`, `double`, `qreal` | JSON number |
| `QString` | JSON string |
| `QPoint`, `QPointF` | `{"x": n, "y": n}` |
| `QSize`, `QSizeF` | `{"width": n, "height": n}` |
| `QRect`, `QRectF` | `{"x": n, "y": n, "width": n, "height": n}` |
| `QColor` | `{"r": n, "g": n, "b": n, "a": n}` (0-255) or `"#RRGGBBAA"` string |
| `QFont` | `{"family": str, "pointSize": n, "bold": bool, "italic": bool, ...}` |
| `QMargins`, `QMarginsF` | `{"left": n, "top": n, "right": n, "bottom": n}` |
| `QDate` | ISO 8601 string `"YYYY-MM-DD"` |
| `QTime` | ISO 8601 string `"HH:MM:SS.mmm"` |
| `QDateTime` | ISO 8601 string |
| `QUrl` | String |
| `enum` / `QFlags` | Integer (for enum), integer or array of integers (for flags) |
| `QVariantList` | JSON array |
| `QVariantMap` | JSON object (string keys only) |
| `QPixmap`, `QImage` | Replaced with `"$binary"` marker in tier "extended"; full `data/` URI in tier "full" |
| `QByteArray` (>1KB) | `"$binary"` marker in extended tier; base64 or file URI in full tier |
| `QObject*` | Serialized as `element_id` integer if the object is in the snapshot; `null` otherwise |
| `QFont`, `QBrush`, `QPen` with pixmap/pattern | `"$binary"` marker |
| Callbacks / function types | Skipped — not serializable |
| Dynamic properties | Included (via `QObject::dynamicPropertyNames()`, not `QMetaObject::property()`) |
| `QKeySequence` | String `"Ctrl+C"` or `{"key": 67, "modifiers": 4}` object |
| `QCursor` | `{"shape": n}` or `{"shape": n, "bitmap": "$binary"}` if custom pixmap cursor |
| `QIcon` | `"$binary"` marker in extended tier; array of `data/` URIs in full tier (one per mode/size) |
| `QPalette` | Omitted in extended tier; JSON object of color groups in full tier |
| `QUuid` | String `"550e8400-e29b-41d4-a716-446655440000"` |
| `QLocale` | JSON object `{language, country, script}` with integer enum values |
| `QJsonValue/Object/Array` (Q_PROPERTY) | Recursive JSON serialization |
| `QEasingCurve` | `{"type": "InOutQuad", "period": 0.3, ...}` string enum + optional params |
| `QVector2D/3D/4D` | `{"x": n, "y": n}` / `{"x": n, "y": n, "z": n}` / `{"x": n, "y": n, "z": n, "w": n}` |
| `QLine/QLlineF` | `{"x1": n, "y1": n, "x2": n, "y2": n}` |
| `QTransform` | `{"m11": n, ..., "m33": n}` as 3×3 affine matrix elements |
| Callbacks / `std::function` / signal handlers | Skipped — not serializable |

---

## 9. Platform-Specific Injection

### 9.1 Interface

```cpp
// injector.h — platform-agnostic
struct InjectResult {
    bool ok;
    std::string error;
};

// Inject library into process. Library startup is deferred (see per-platform notes).
InjectResult inject(int pid, const std::string& lib_path);

// Gracefully eject: sends shutdown RPC, joins threads, THEN calls OS unload API.
// Never calls FreeLibrary/dlclose while library threads are running.
void eject(int pid, const std::string& lib_path);
```

### 9.2 Windows

**Technique:** `CreateRemoteThread` + `LoadLibraryW`

```
OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION)
  → VirtualAllocEx()            // allocate memory for DLL path
  → WriteProcessMemory()        // write DLL path string (must be < MAX_PATH)
  → CreateRemoteThread(..., LoadLibraryW, ...)  // trigger load
```

- **Architecture matching is mandatory:** 64-bit injector cannot inject into 32-bit process and
  vice versa. The server must check target process architecture before injection and fail with a
  clear error. Architecture is reported by `qt_list_processes`.
- **DllMain restriction:** `DllMain(DLL_PROCESS_ATTACH)` MUST NOT create threads, call socket APIs,
  or allocate memory. Instead, it sets an atomic flag `library_loaded = true`.
- **Deferred initialization:** After `LoadLibraryW` returns, the injector captures the DLL base
  from the remote thread's exit code (`GetExitCodeThread`). It then:
  1. Parses the PE export directory of the DLL file on disk to find the RVA of `qt_commander_init`.
  2. Computes `target_address = dll_base + rva`.
  3. Allocates and writes the `InitParams` struct (see Section 9.3) into the target process.
  4. Calls `CreateRemoteThread(target, target_address, init_params_ptr)` to execute the deferred init.
  This two-step approach avoids all DllMain restrictions.
- **PE export parsing:** The injector reads the DLL file from disk, locates the IMAGE_EXPORT_DIRECTORY,
  and walks the name/ordinal/address tables to find `qt_commander_init`. The RVA is added to the
  DLL base (obtained from `GetExitCodeThread` on the LoadLibraryW thread) to compute the absolute
  address in the target process. The parsed RVA is also needed for future `FreeLibrary` eject calls.
- **HMODULE capture:** `GetExitCodeThread(hLoadLibraryThread, &hModule)` retrieves the DLL base.
  This handle must be stored for the eject phase (`FreeLibrary` call).
- A second thread runs `qt_commander_init`; the third runs `FreeLibrary`.
- Every `CreateRemoteThread` handle must be waited on (`WaitForSingleObject`) and then
  closed (`CloseHandle`) to prevent kernel object leaks. All three thread handles are tracked
  and managed: `hLoadLibrary`, `hInit`, `hFreeLibrary`.
- Debug DLLs (`Qt5Cored.dll`) are also checked during process detection.

### 9.3 Linux

**Technique:** `ptrace` + dynamic dlopen

```
ptrace(PTRACE_ATTACH) + waitpid()
  → Locate __libc_dlopen_mode in target's libc
    1. Read /proc/<pid>/maps to find libc base address
    2. Read the target's libc ELF headers via process_vm_readv or ptrace PEEKDATA
    3. Parse .dynsym to find __libc_dlopen_mode's address = base + offset
  → Allocate memory in target (find free region via /proc/<pid>/maps)
     → Write .so path string to allocated region
     → Write small (~100 byte) architecture-specific shellcode to separate region
  → PTRACE_GETREGS: save register state
  → PTRACE_SETREGS: set RIP to shellcode, RDI to .so path ptr
  → PTRACE_CONT + waitpid() for shellcode to execute dlopen + trap back
  → PTRACE_GETREGS: read RAX for dlopen return value (handle)
  → PTRACE_SETREGS: restore original register state
  → PTRACE_DETACH
```

- **Requires:** `CAP_SYS_PTRACE` or root, or `ptrace_scope=0` (`/proc/sys/kernel/yama/ptrace_scope`).
- **libc compatibility:** `__libc_dlopen_mode` is a glibc internal symbol. Systems using musl libc
  (Alpine Linux) or uClibc are **not supported**.
- **SELinux/AppArmor:** May block ptrace even with Yama disabled. Document as a known limitation.
- **Shellcode:** The shellcode performs `call __libc_dlopen_mode(path, RTLD_NOW)` then executes
  a `int3` trap to return control to the injector. Architecture-specific: x86_64 uses `sysv`
  calling convention; ARM64 uses AAPCS64. No dynamic linking — all offsets are resolved at
  ptrace time by reading the target's ELF.
- **Deferred init:** After dlopen returns the .so handle, the same ptrace sequence is repeated
  to call `qt_commander_init(init_params_ptr)` which starts the RPC thread.
- `.so` constructor (`__attribute__((constructor))`) does NOTHING except set an atomic flag.
- For eject: `dlclose` via the same ptrace mechanism, but only after shutdown RPC confirms
  all threads stopped.
- x86_64 and ARM64 supported.

### 9.4 Cross-Platform Port Handshake

The port handshake mechanism for **running processes** does NOT use environment variables
(which cannot be modified on a running process). Instead, the injector allocates and writes
an `InitParams` structure into the target process memory.

**InitParams struct (versioned, offset-based, architecture-independent):**

```c
// All fields stored inline at fixed offsets. Total size: 1024 bytes.
// Pad bytes ensure 8-byte alignment. This struct is written directly to
// the target process memory via WriteProcessMemory / mach_vm_write / ptrace POKEDATA.
// The target library casts the received pointer to InitParams and reads fields.

#include <stdint.h>

#define INIT_PARAMS_VERSION 1
#define INIT_PARAMS_MAX_PATH 256
#define INIT_PARAMS_TOKEN_LEN 64
#define INIT_PARAMS_TOTAL_SIZE 1024

typedef struct {
    uint32_t version;          // offset 0: struct version (=1)
    uint32_t total_size;       // offset 4: total struct size (=1024)
    char     workspace_path[INIT_PARAMS_MAX_PATH];  // offset 8
    char     session_id[13];   // offset 264: 12 chars + null
    char     token[INIT_PARAMS_TOKEN_LEN + 1];      // offset 277: 64 hex chars + null
    char     port_file_path[INIT_PARAMS_MAX_PATH];  // offset 342
    uint8_t  reserved[426];    // offset 598: padding to 1024 bytes (future use)
} InitParams;
// static_assert(sizeof(InitParams) == 1024, "InitParams size mismatch");
```

**Port handshake sequence:**

```
MCP Server                                    Qt Process
    │                                              │
    ├── 1. Create session dir, generate token       │
    │      <workspace>/sessions/<id>/port.txt       │
    │                                              │
    ├── 2. Allocate 1024 bytes in target            │
    │      Fill InitParams with version=1,           │
    │      workspace_path, session_id, token,        │
    │      port_file_path                            │
    │      WriteProcessMemory / mach_vm_write /      │
    │      ptrace POKEDATA                           │
    │                                              │
    ├── 3. Inject DLL/SO/DYLIB ──────────────────►│
    │                                              │
    ├── 4. Call qt_commander_init(params_ptr) ────►│
    │                                              │
    │         5. Library validates version==1        │
    │         6. tcp_listen_loopback(0) → port       │
    │         7. Write "port\ntoken\n" to port_file  │
    │            (exact bytes: no \r on any platform)│
    │            (atomic: write to .tmp, rename)     │
    │            (O_CREAT | O_EXCL / CREATE_NEW)     │
    │            (file perms: 0600 / owner-only DACL)│
    │                                              │
    ├── 8. Poll port.txt (exponential backoff:      │
    │      50ms → 100ms → 200ms → 400ms → 800ms     │
    │      → 1.6s → 3.2s. Total ~6s, ~15s max.)    │
    │      Only read when file size is stable        │
    │      across 2 consecutive polls.               │
    ├── 9. Read 2 lines: port (numeric) + token      │
    ├── 10. tcp_connect_loopback(port) ────────────►│
    ├── 11. Send auth frame (Section 2.5) ─────────►│
    │                                              │
    │  ◄──────── TCP connected + auth OK ──────────►│
    │                                              │
    ├── 12. Library deletes port.txt after auth     │
```

**String encoding in port file:** All strings are written as literal bytes with `\n` line
separators (always `0x0A`, never `\r\n`). The token is guaranteed to contain only hex chars
`[0-9a-f]`, so no special characters or newlines appear in the data.

### 9.5 macOS

**Technique:** Mach API — `task_for_pid` + `mach_vm_allocate` + `thread_create_running`

**Critical caveats (corrected from v1):**
- `task_for_pid()` is restricted by macOS security, NOT just SIP. Even self-compiled,
  non-Apple-signed applications are protected. The API requires `com.apple.security.cs.debugger`
  entitlement or running as root with specific authorization.
- **Hardened Runtime:** macOS 10.14+ enables this by default. It prevents `dlopen`/injection
  unless the target has `com.apple.security.cs.disable-library-validation` entitlement.
- **W^X on ARM64:** `mach_vm_protect(RWX)` is hardware-enforced to FAIL. Instead, allocate
  RW memory → write shellcode → `mach_vm_protect(RX)` (not RWX).
- **Code signing:** Both injector and injected `.dylib` must be code-signed (ad-hoc signing
  with `codesign --sign -` works for non-distribution use).
- The init_params memory-passing mechanism is used here as well (see Section 9.4 diagram).

**macOS is the most restricted platform.** Injection into self-compiled Qt apps is possible
with ad-hoc signing but requires opt-in entitlements or system configuration. Document these
requirements clearly.

### 9.6 TCP Socket Abstraction

```cpp
// socket_utils.h — shared by server and library
using socket_t = uintptr_t;  // SOCKET on Windows, int on POSIX
static const socket_t INVALID_SOCK = ~(socket_t)0;

socket_t tcp_listen_loopback(uint16_t& port);    // bind 127.0.0.1:0, return actual port
socket_t tcp_accept(socket_t listen_fd);
socket_t tcp_connect_loopback(uint16_t port);
void    tcp_close(socket_t fd);
bool    tcp_send_all(socket_t fd, const void* data, size_t len);  // loops until all sent
bool    tcp_recv_all(socket_t fd, void* buf, size_t len);         // loops until all recvd
void    tcp_set_recv_timeout(socket_t fd, int timeout_ms);        // SO_RCVTIMEO
void    tcp_set_send_timeout(socket_t fd, int timeout_ms);        // SO_SNDTIMEO
```

- Windows requires `WSAStartup()` once at process start (RAII guard). Library calls it in
  `qt_commander_init()` (not DllMain).
- `tcp_send_all` / `tcp_recv_all` loop internally handling partial sends/receives and `EINTR`.
- `socket_t` avoids truncation of `SOCKET` (which is `UINT_PTR`) on 64-bit Windows.

---

## 10. Threading Model (Injected Library)

### 10.1 Design

The worker thread dispatches operations to the main thread using **manual `Qt::QueuedConnection`
+ `QSemaphore::tryAcquire` + `QTimer` timeout**. This replaces the v2 `BlockingQueuedConnection`
approach, which has no timeout parameter in Qt's API.

```
Worker Thread (RPC Server)              Main Thread (Qt Event Loop)
       │                                        │
       ├─ recv length-prefixed frame             │
       ├─ parse JSON-RPC request                 │
       ├─ acquire read lock on map_lock_         │
       ├─ validate element_id exists             │
       ├─ capture current_epoch_                 │
       ├─ release read lock                      │
       │                                        │
       ├─ Create QSemaphore sem(0)               │
       ├─ QMetaObject::invokeMethod(            │
       │      mainObj, &doHandle,               │
       │      Qt::QueuedConnection,              │  ← NON-BLOCKING dispatch
       │      Q_ARG(quint64, element_id),        │
       │      Q_ARG(QSemaphore*, &sem),          │
       │      Q_ARG(quint64, captured_epoch),    │
       │      Q_ARG(QVariant*, &result), ...)    │
       │                                        │
       ├─ if (!sem.tryAcquire(1, 30000)) {      │  ← 30s timeout
       │      return error code 2004              │
       │  }                                      │
       │                                        ├─ doHandle() runs on main thread
       │                                        ├─ acquire read lock, check epoch
       │                                        ├─ check element valid (nullptr guard)
       │                                        ├─ perform Qt widget operation (try/catch)
       │                                        ├─ store result in QVariant*
       │                                        ├─ release read lock
       │                                        └─ sem.release(1)
       │                                        │
       │  ◄────── sem acquired ────────────────┤
       │                                        │
       ├─ serialize *result to JSON             │
       ├─ encode length-prefixed frame          │
       └─ tcp_send_all(response)                │
```

**Why not BlockingQueuedConnection?** `Qt::BlockingQueuedConnection` has no timeout parameter.
It blocks the calling thread forever until the slot completes. If the main thread is hung
(busy loop, no event processing), the worker thread hangs permanently. The manual semaphore
approach provides a 30-second timeout, returning error code 2004 on expiry.

### 10.2 Key Design Rules

| Rule | Reason |
|------|--------|
| **Manual `Qt::QueuedConnection` + `QSemaphore::tryAcquire(30000)`** | Provides 30s timeout; BlockingQueuedConnection has no timeout. |
| **QTimer fallback for timeout** | If main thread is alive but slow, the 30s semaphore wait sets an upper bound on RPC latency. |
| **QReadWriteLock protects `element_map_`** | Worker (read lock), main thread snapshot (write lock). |
| **Epoch check on main thread** | Worker captures epoch; main thread re-checks after acquiring read lock. Epoch mismatch → reject (code 1002). |
| **Iterative snapshot traversal** | `QStack<QObject*>` + cycle detection + max depth 1000. Prevents stack overflow. |
| **All main-thread callbacks wrapped in try/catch** | Prevents `qFatal()` from killing the process. Catches `std::exception`; returns error responses. |
| **Single TCP connection** | After `accept()`, listen socket is closed. Only one MCP server connection allowed. Second connection → refused. |
| **Keepalive via `SO_KEEPALIVE` with 5s interval** | Library detects server death (TCP RST/timeout). Server detects library death (connection closed). |
| **Nullptr guard at point of use** | Even after epoch check, the QObject pointer is re-verified immediately before the Qt API call. See Section 10.5. |

### 10.3 Re-entrancy Guard

When the main thread is in a nested event loop (`QDialog::exec()`, `QMenu::exec()`,
`QQuickItemGrabResult` async wait), `Qt::QueuedConnection` dispatches WILL be processed.
The library uses an atomic busy flag:

```cpp
std::atomic<bool> operation_in_progress_{false};
```

Before dispatching, the worker sets this flag. If a second request arrives while the flag is set,
the worker returns error code 2007. The MCP server serializes per-session requests to prevent this
on the client side.

### 10.4 QML Event Delivery

Mouse/keyboard events for QQuickItems MUST go through `QQuickWindow::sendEvent()`, not
`QCoreApplication::postEvent()`. This ensures:
- Scene-graph hit testing (respects clipping, Z order, `contains()`)
- `MouseArea` signal generation (`onClicked`, `onPressed`)
- Proper coordinate transformation (scene → local)

The event injector detects element type at dispatch time using `qobject_cast`:
```cpp
if (auto* widget = qobject_cast<QWidget*>(obj)) { /* QWidget path */ }
else if (auto* item = qobject_cast<QQuickItem*>(obj)) {
    QQuickWindow* win = item->window();  // Qt 5.0+
    if (win) win->sendEvent(item, event);  // Scene-graph dispatch path
}
```

### 10.5 Nullptr Guard at Point of Use

Even after epoch validation, the QObject can be destroyed between the check and the
actual Qt API call (e.g., when the operation triggers `processEvents()` internally).
Every main-thread callback re-validates the raw pointer immediately before use:

```cpp
void doHandle(quint64 id, QSemaphore* sem, quint64 captured_epoch, QVariant* result) {
    QReadLocker lock(&map_lock_);
    if (captured_epoch != current_epoch_) { sem->release(1); return; }
    QObject* obj = element_map_.value(id, nullptr);
    if (!obj) { sem->release(1); return; }
    // ... perform Qt operation with obj ...
    // NOTE: do NOT call processEvents() or enter nested loops after this point
    // without re-validating obj
    *result = ...;
    lock.unlock();
    sem->release(1);
}
```

### 10.6 QML Visibility in Snapshots

`QQuickItem::isVisible()` only checks the item's own `visible` property — it does NOT
check ancestor visibility (unlike `QWidget::isVisible()`). The scanner must walk the
`parentItem()` chain and check effective visibility:

```cpp
bool isEffectivelyVisible(QQuickItem* item) {
    for (QQuickItem* p = item; p; p = p->parentItem()) {
        if (!p->isVisible()) return false;
    }
    return true;
}
```

This prevents hidden-by-ancestor QML items from appearing in `include_hidden=false` snapshots.

### 10.7 QQuickItem Screenshot (Async)

`QQuickItem::grabToImage()` is asynchronous (returns `QQuickItemGrabResult*`; result delivered
via signal). To use in the synchronous dispatch model, the main-thread callback:
1. Calls `item->grabToImage()` — creates a `QQuickItemGrabResult`.
2. Connects `result->ready()` to a local `QEventLoop::quit()`.
3. Calls `loop.exec()` (nested event loop) — waits for the signal.
4. Captures the image from the result.
5. Exits the nested loop.

This is safe because the re-entrancy guard (Section 10.3) prevents concurrent operations.
The nested event loop processes only Qt system events and the grab result signal —
no new RPC operations enter.

---

## 11. Qt Process Detection

### 11.1 Detection Method

| Platform | API | Libraries Checked | Debug Suffix Support |
|----------|-----|-------------------|---------------------|
| Windows | `EnumProcessModules` + `GetModuleBaseNameW` | `Qt5Core.dll`, `Qt5Cored.dll`, `Qt6Core.dll`, `Qt6Cored.dll` | Yes (checks both) |
| Linux | `/proc/<pid>/maps` glob: `libQt*Core.so*` | Matches `libQt5Core.so.5`, `libQt5Core.so.5.15.2`, `libQt6Core.so.6`, etc. | Not applicable (no debug suffix convention) |
| macOS | `mach_vm_region` / parsing `vmmap` output | `QtCore.framework/Versions/*/QtCore` | Framework-based, no debug suffix |

**Static linking exclusion:** If no Qt shared library is loaded, the process is not detected.
This is a hard limitation. Most Qt applications use dynamic linking.

### 11.2 Window Title Extraction

| Platform | Method | Best-Effort? |
|----------|--------|-------------|
| Windows | `EnumWindows` + `GetWindowTextW`, filter by PID | Yes (works reliably) |
| Linux/X11 | `XGetWindowProperty(_NET_WM_NAME)` | Yes |
| Linux/Wayland | **Not possible** — Wayland protocol prohibits cross-client window enumeration | No — returns empty |
| macOS | `CGWindowListCopyWindowInfo` (requires Screen Recording permission in 10.15+) | Yes (requires permission) |

---

## 12. Build System

### 12.1 CMake Strategy

```cmake
cmake_minimum_required(VERSION 3.16)
project(qt-commander LANGUAGES CXX)

option(BUILD_SERVER      "Build qt-commander MCP server"  ON)
option(BUILD_LIBRARY     "Build injection library"        OFF)
set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")

if(BUILD_SERVER)
    add_subdirectory(src/server)
endif()

if(BUILD_LIBRARY)
    if(QT_MAJOR_VERSION EQUAL 6)
        find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Quick Qml)
    else()
        find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Quick)
    endif()
    add_subdirectory(src/library)
endif()
```

Note: Qt6 splits Quick and Qml into separate CMake components. Both are required for QML support.

### 12.2 Build Flows

```
# Build server only (minimal deps, quick iteration):
cmake -B build -DBUILD_LIBRARY=OFF
cmake --build build

# Build library (Windows):
call vcvars64.bat
call qtenv2.bat
cmake -B build/lib -DBUILD_SERVER=OFF -DBUILD_LIBRARY=ON -DQT_MAJOR_VERSION=5
cmake --build build/lib --parallel

# AI-triggered build:
qt_build_library(
  qt_env="C:\Software\Qt\5.15.2\msvc2019_64\bin\qtenv2.bat",
  vcvars="C:\Software\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
```

### 12.3 Build Manager Logic

1. Receives env script paths. On Windows, builds a single `.bat` wrapper that chains vcvars + qtenv
   in the same `cmd.exe` session (environment variables must propagate across chained calls).
2. Copies `src/library/` to `<workspace>/builds/<key>/src/` (build key now includes Qt version +
   content hash to avoid stale source issues).
3. Runs CMake configure + build in `<workspace>/builds/<key>/build/`.
4. Returns path to compiled library.

**Build tool auto-detection order:** jom → ninja → nmake (Windows), ninja → make (Linux/macOS).
User override via `generator` parameter.

**Concurrent builds:** A file lock (`LockFileEx` on Windows, `flock` on POSIX) serializes
builds for the same key.

---

## 13. Test Strategy

Test frameworks: **Catch2 v2** (header-only `catch.hpp`) for server unit tests;
**QTestLib** for library tests (links against `Qt5Test`/`Qt6Test`).

### 13.1 Unit Tests — Server (`tests/unit_server/`)

| Suite | File | Key Cases |
|-------|------|-----------|
| `test_json_rpc` | `test_json_rpc.cpp` | Full JSON-RPC lifecycle; invalid JSON; batch; malformed input |
| `test_framing` | `test_framing.cpp` | Valid frame encode/decode; partial frames; length=0 rejection; length>16MB rejection; magic byte validation; version byte rejection; multiple frames per buffer; garbage data |
| `test_session_manager` | `test_session_manager.cpp` | Create/list/detach session; PID dedup rejection; concurrent multi-session; orphan scan on startup |
| `test_build_manager` | `test_build_manager.cpp` | Build key derivation; env script chain construction; path resolution; build cache; negative: missing script, cmake failure, disk full |
| `test_socket_utils` | `test_socket_utils.cpp` | TCP listen random port; connect loopback; send_all/recv_all round-trip; accept; timeout; graceful close |
| `test_transport` | `test_transport.cpp` | stdio: send/receive JSON-RPC; HTTP: SSE event formatting; MCP initialize handshake |
| `test_injector` | `test_injector.cpp` (platform-conditional) | Process not found; access denied; invalid path; architecture mismatch; injection timeout |
| `test_process_detector` | `test_process_detector.cpp` | Detect known Qt process; reject non-Qt; no process running; permission denied |

### 13.2 Unit Tests — Library (`tests/unit_library/`)

| Suite | File | Key Cases |
|-------|------|-----------|
| `test_selector` | `test_selector.cpp` | type exact; type_inherits IS-A; text/text_contains; object_name; window_title; property filters; depth modes; limit; empty query; AND combination |
| `test_element_map` | `test_element_map.cpp` | Assign ID; lookup; stale ID; clear + rebuild; epoch increment; thread-safe read-lock/write-lock; duplicate insert |
| `test_ui_scanner` | `test_ui_scanner.cpp` | Widget tree; QQuickItem tree; mixed Widget+QML; transient popups; cycle detection (verify truncation); max depth 1000 (verify truncation); property serialization all types; $binary markers; empty widget tree (element_count=0); QQuickWindow::contentItem() null; QCoreApplication-only (no widgets); negative coordinates; zero-width/height elements; QML item with visible=true inside hidden parent (effective visibility) |
| `test_event_injector` | `test_event_injector.cpp` | Mouse events all types; key events all types; key name parsing; touch (QTouchDevice auto-register); focus via setFocus/clearFocus; modifier handling |
| `test_rpc_handler` | `test_rpc_handler.cpp` | Method dispatch; unknown method; invalid params; auth token validation + wrong-token rejection + no-auth-close-after-5s; element destroyed/stale/not-visible/disabled/zero-size errors; epoch mismatch rejection |
| `test_screenshot` | `test_screenshot.cpp` | Widget grab; QQuickItem grabToImage async (QEventLoop wait); null element; zero-size element; offscreen/negative-coord element |

### 13.3 Integration Tests (`tests/integration/`)

Require `tests/qt-test-app/` — a minimal Qt app with:
- QMainWindow (menu bar, toolbar, status bar)
- QPushButton ("OK", "Cancel")
- QLineEdit (with placeholder text)
- QCheckBox, QComboBox (with items)
- QTabWidget (2 tabs)
- QDialog (modal, with OK/Cancel)
- QLabel (with text)
- QQuickView + QML scene (Rectangle, Text, TextInput, MouseArea, ListView)
- Dynamic element create/destroy
- QSpinBox, QSlider

| Suite | Key Cases |
|-------|-----------|
| `test_integration` | Full lifecycle: attach → snapshot → find → property r/w → click → type → screenshot → detach |
| | Multi-session isolation |
| | Element lifecycle (create → ID → delete → stale) |
| | UI change detection (snapshot v1 → modify → v2) |
| | Keyboard type into QLineEdit → verify text |
| | ComboBox selection via set_property currentIndex |
| | Tab switch via call_method setCurrentIndex |
| | QML MouseArea click via QQuickWindow::sendEvent path |
| | Modal dialog: verify operations work during QDialog::exec() |
| | Screenshot: verify PNG non-empty |
| | Error: stale ID; destroyed element; invisible element; disabled element; zero-size element; operation-in-progress (2007); auth failed (2009); invalid session_id (-32602) |
| | Epoch-based snapshot invalidation |
| | Snapshot truncation: cycle simulated; max depth exceeded; empty tree (element_count=0) |

### 13.4 Concurrent/Stress Tests

| Case | Description |
|------|-------------|
| Snapshot + click serialization | Second request rejects with code 2007 if operation in progress |
| Snapshot + snapshot serialization | Two concurrent qt_snapshot calls — second returns 2007 |
| PID dedup | Second qt_attach to same PID rejects with code 2006 |
| TCP disconnect recovery | Server detects connection loss, marks session zombie |
| Library detects server death | TCP keepalive triggers, library cleans up RPC thread |
| Two sessions concurrent | Independent sessions do not interfere; snapshots/screenshots are isolated |
| detach during snapshot | Detach RPC arrives while snapshot is writing; graceful shutdown |
| Server killed during recv | Library detects broken TCP, stops accepting requests, exits RPC thread |
| Frame length=0 / >16MB | Library rejects malformed frames with error code 2008, closes connection |

### 13.5 Platform Coverage

| Platform | Priority | Setup |
|----------|----------|-------|
| Windows | Primary | VS 2022 + Qt 5.15.2 / Qt 6.8.3, jom for parallel builds |
| Linux | Secondary | GCC/Clang, Qt from system package manager |
| macOS | Tertiary | Clang, Qt from Homebrew, ad-hoc code signing |

---

## 14. Resolved Design Decisions

1. **Port handshake:** Use injected-memory init_params struct, NOT environment variables.
   Environment variables cannot be set on a running process.
2. **Thread safety:** `QReadWriteLock` + raw `QObject*` (not `QPointer`, which is not thread-safe).
3. **TCP framing:** Length-prefixed frames (4-byte big-endian length + payload).
4. **Channel auth:** Per-session 32-byte random token passed via init_params + port file.
5. **MCP resources:** Proper `resources/list` and `resources/read` instead of ad-hoc `$ref`.
6. **Deferred init:** Library has a separate `qt_commander_init()` exported function that
   must be called after `DllMain`/constructor. Thread creation happens there, not in DllMain.
7. **QML events:** Route through `QQuickWindow::sendEvent()` for proper MouseArea handling.
8. **Drag-and-drop:** Deferred to future version — Qt's `QDrag::exec()` blocks and requires
   re-entrant event loop management incompatible with the semaphore dispatch model.
9. **qt_activate:** Removed — no universal Qt activation mechanism exists. AI uses type-specific
   methods (click, call_method, set_property).
10. **Snapshot detail tiers:** Core/Extended/Full to manage snapshot size (avoid 5 MB+ JSON).
11. **Snapshot traversal:** Iterative (`QStack`) + cycle detection + max depth guard (1000 levels).
12. **Session ID:** Random 12-char alphanumeric `[a-z0-9]` from `std::random_device`.
13. **Build tool detection:** jom → ninja → nmake (Windows), ninja → make (Unix).
    User overrides via `generator` parameter.
14. **Build source freshness:** Build key includes source content hash.
15. **Snapshot numbering:** 8-digit zero-padded, sequential per session.
16. **Error codes:** MCP `-32000` range with `data.code` sub-codes (100x element, 200x system).
17. **Concurrency:** Per-session request serialization via atomic busy flag. PID dedup in session manager.
18. **Exception safety:** All library main-thread callbacks wrapped in try/catch. Worker thread loop
    is exception-safe to prevent thread death.
19. **Framing format:** 6-byte header (magic 0xCC + version 0x01 + 4-byte BE length). Max 16 MB.
    Length=0 and length>16MB cause connection close with error.
20. **Auth protocol:** First framed JSON-RPC request after TCP connect must be method
    `"qt.authenticate"` with token param. Wrong token → error + close. No auth within 5s → close.
21. **Port file format:** Two lines: port (ASCII decimal) then token (64 hex chars), separated by
    `\n` (always `0x0A`, never `\r\n`). File created with `O_CREAT|O_EXCL` with `0600`/owner-only
    DACL permissions. Deleted after successful auth.
22. **InitParams struct:** Fixed-layout 1024-byte struct with version and total_size fields.
    Strings are inline char arrays (256-byte paths, 13-byte session_id, 65-byte token). No pointers
    across process boundary. Reserved padding for forward compatibility.
23. **Dispatch mechanism:** Manual `Qt::QueuedConnection` + `QSemaphore::tryAcquire(1, 30000)`
    for 30s RPC timeout. `Qt::BlockingQueuedConnection` is NOT used because it has no timeout
    parameter and would hang forever on a blocked main thread.
24. **Snapshot truncation:** `truncated: true` field with reason string when cycle detected or
    max depth (1000) reached. Empty tree (`element_count: 0`) is valid.
25. **QML visibility:** Effective visibility check walks `parentItem()` chain (not just
    `isVisible()`, which QQuickItem checks only on itself).
26. **QML screenshot async:** `QQuickItem::grabToImage()` handled via `QEventLoop` nested in
    the main-thread callback, protected by the re-entrancy guard.

---

## 15. Project Structure

```
qt-commander/
├── CMakeLists.txt
├── cmake/
│   └── QtCommanderConfig.cmake
├── src/
│   ├── server/                       # qt-commander executable
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp
│   │   ├── mcp/
│   │   │   ├── transport.h           # Transport interface (init, send, recv, close)
│   │   │   ├── transport_stdio.h
│   │   │   ├── transport_stdio.cpp   # stdio transport
│   │   │   ├── transport_http.h
│   │   │   ├── transport_http.cpp    # HTTP/SSE transport
│   │   │   ├── protocol.h            # JSON-RPC parsing + framing
│   │   │   └── protocol.cpp
│   │   ├── session/
│   │   │   ├── session.h             # Single Qt process session
│   │   │   ├── session.cpp
│   │   │   ├── session_manager.h     # Multi-session state
│   │   │   └── session_manager.cpp
│   │   ├── inject/
│   │   │   ├── injector.h            # Platform-agnostic interface
│   │   │   ├── injector_win.cpp      # Windows: CreateRemoteThread + deferred init
│   │   │   ├── injector_linux.cpp    # Linux: ptrace + dlopen
│   │   │   └── injector_macos.cpp    # macOS: Mach API
│   │   ├── process/
│   │   │   ├── process_detector.h    # Qt process enumeration
│   │   │   ├── process_detector.cpp
│   │   │   ├── process_detector_win.cpp
│   │   │   ├── process_detector_linux.cpp
│   │   │   └── process_detector_macos.cpp
│   │   └── build/
│   │       ├── build_manager.h
│   │       └── build_manager.cpp
│   ├── library/                      # libqt-commander (injected)
│   │   ├── CMakeLists.txt
│   │   ├── api.h                     # Exported qt_commander_init() declaration
│   │   ├── entry_win.cpp             # DllMain (flag only) + qt_commander_init (RPC start)
│   │   ├── entry_unix.cpp            # constructor (flag only) + qt_commander_init
│   │   ├── compat_qt.h               # Qt5/Qt6 compatibility macros
│   │   ├── rpc/
│   │   │   ├── rpc_server.h
│   │   │   └── rpc_server.cpp        # TCP framed JSON-RPC server + auth
│   │   ├── core/
│   │   │   ├── ui_scanner.h
│   │   │   ├── ui_scanner.cpp        # Widget + QML tree traversal (iterative)
│   │   │   ├── event_injector.h
│   │   │   ├── event_injector.cpp    # Qt event synthesis (routing logic)
│   │   │   ├── element_map.h
│   │   │   ├── element_map.cpp       # QReadWriteLock-protected ID mapping
│   │   │   ├── screenshot.h
│   │   │   └── screenshot.cpp        # Widget/QML/cross-platform screenshot
│   │   ├── selector/
│   │   │   ├── selector.h
│   │   │   └── selector.cpp          # JSON query → element matching
│   │   └── protocol/
│   │       ├── handler.h
│   │       └── handler.cpp           # JSON-RPC method dispatch + error codes
│   └── common/                       # Shared between server and library
│       ├── socket_utils.h            # TCP abstraction (socket_t, send_all, recv_all)
│       ├── socket_utils.cpp
│       ├── framing.h                 # Length-prefixed frame encode/decode
│       └── framing.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── qt-test-app/                  # Integration test Qt application
│   │   ├── CMakeLists.txt
│   │   ├── main.cpp
│   │   ├── mainwindow.h / .cpp
│   │   ├── test_dialog.h / .cpp
│   │   └── test_scene.qml
│   ├── unit_server/
│   │   ├── CMakeLists.txt
│   │   ├── test_json_rpc.cpp
│   │   ├── test_session_manager.cpp
│   │   ├── test_build_manager.cpp
│   │   ├── test_socket_utils.cpp
│   │   ├── test_transport.cpp
│   │   ├── test_injector.cpp
│   │   ├── test_process_detector.cpp
│   │   └── test_framing.cpp
│   ├── unit_library/
│   │   ├── CMakeLists.txt
│   │   ├── test_selector.cpp
│   │   ├── test_element_map.cpp
│   │   ├── test_ui_scanner.cpp
│   │   ├── test_event_injector.cpp
│   │   ├── test_rpc_handler.cpp
│   │   └── test_screenshot.cpp
│   └── integration/
│       ├── CMakeLists.txt
│       └── test_integration.cpp
└── external/
    └── nlohmann/
        └── json.hpp                  # Header-only JSON (server only)
    └── catch2/
        └── catch.hpp                 # Catch2 v2 (header-only, server tests only)
```
