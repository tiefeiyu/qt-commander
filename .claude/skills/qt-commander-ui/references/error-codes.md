# qt-commander Error Code Reference

qt-commander MCP tools report failures with a `data.code` sub-code. This table lists every code and how to handle it.

| Code | Meaning | Handling |
|---|---|---|
| 1001-1005 | Element missing/stale/not visible/disabled/zero-size | Re-find/snapshot to get a fresh id |
| 2001 | Build missing or failed | Run qt_build; if the MCP process runs stale logic, restart the MCP server |
| 2002 | Injection failed | Check build parameters against the target process (qt_major/toolchain/build_type/bitness), see build-params.md |
| 2003 | Target process unresponsive | The app may be stuck; wait 5s and retry once, or investigate the app |
| 2004 | Main-thread operation timed out | **The operation may still execute (at-least-once) — do not blindly retry side-effect operations**; snapshot first to verify state |
| 2006 | Already attached | qt_detach first, then attach |
| 2007 | Main thread busy in a nested event loop | **Definitively not executed — safe to retry** |
| 2008 | Frame too large (>16MB) | Shrink the request (e.g. snapshot max_depth/detail) |
| 2009 | Authentication failed | Re-attach |
| 2010 | Snapshot truncated | Retry with smaller max_depth/detail |
| 2011 | Session lost (process exited / connection dropped) | Re-attach |

## 2007 vs 2004 (important)

| | 2004 | 2007 |
|---|---|---|
| Meaning | Wait timed out; the request **may have executed** | **Definitively not executed** (rejected by the reentrancy guard) |
| Retry | Snapshot first to verify state; do not blindly retry side-effect ops | Safe to retry |
| Typical cause | Main thread busy (startup/page load) longer than 30s | Concurrent request during a nested event loop (e.g. screenshot grab) |
