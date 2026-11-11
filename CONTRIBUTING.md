# Contributing

Thanks for taking the time. This project is a Python MCP server plus a C++
runtime that runs inside the target application, so small changes can span both
languages — the notes below say where things live and what a reviewer will look
for.

## Requirements

- Python 3.10+ and [uv](https://docs.astral.sh/uv/)
- CMake 3.16+ and a C++17 compiler, only if you touch `src/`
- Qt 5.15 or 6.5+ (kits for both are fine; `qt-commander doctor` reports what is
  installed)

## Setup

```bash
uv sync                     # create .venv and install dev dependencies
uv run pytest -q            # 148 tests, no Qt required
uv run qt-commander doctor  # what this machine offers
```

For C++ work, build the runtime once:

```bash
cmake -S . -B build/dev -G Ninja -DQT_MAJOR_VERSION=5 -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev
uv run python scripts/package_artifacts.py --build-dir build/dev \
    --qt-version 5.15.2 --toolchain msvc --qt-release
```

or let the agent build it with `qt_build(for_pid=<pid>)` against a running app.

## Where things live

| Path | What it is |
|---|---|
| `qt_commander/server.py` | MCP tool definitions, instructions, build/inject orchestration |
| `qt_commander/ops.py` | The Python side of each operation (polling, expectations, batching) |
| `qt_commander/client.py`, `protocol.py` | Framed JSON-RPC client and wire types |
| `qt_commander/target.py` | Process/Qt-runtime evidence, loaded injector detection |
| `qt_commander/artifacts.py` | Artifact layout, manifest and matching rules |
| `qt_commander/environment.py` | Qt kit and toolchain discovery |
| `qt_commander/cli.py` | `qt-commander serve/doctor/install-artifacts` |
| `src/library/` | The runtime injected into (or linked into) the target |
| `src/injector/` | Native `qtc-inject` (Windows remote thread / Linux ptrace) |
| `tests/test-apps/` | Small Qt apps used by the E2E scripts |
| `scripts/` | E2E, benchmark and packaging scripts |

## Adding or changing an MCP tool

A tool usually touches four places:

1. `qt_commander/ops.py` — the client-side logic (validation, polling, budgets).
2. `qt_commander/server.py` — the `@mcp.tool` wrapper, its docstring (this is
   the agent-facing contract) and the error mapping.
3. `src/library/server.cpp` — only if the target has to do new work.
4. `tests/unit_server/` — a unit test for the Python logic, and an update to
   `EXPECTED_TOOLS` / the description assertions in `test_prompts.py` if the
   tool surface changed.

Guidelines that reviewers enforce:

- Failures are structured (`code`, `cause`, `retryable`, `hint`, `details`),
  never a bare string.
- Every response is budgeted: no unbounded trees, images or lists.
- New defaults must not silently guess. When the evidence is ambiguous, report
  the candidates and ask the caller to choose.
- Docs and the tool description are part of the change, not a follow-up.

## Commits and branches

- Work on a topic branch; open a pull request against `master`.
- Commit messages: English, imperative, Conventional Commits
  (`feat(scope): ...`, `fix(scope): ...`, `docs: ...`, `build: ...`,
  `refactor: ...`, `test: ...`, `chore: ...`). Body explains *why*.
- One logical change per commit; tests in the same commit as the behaviour.
- `CHANGELOG.md` gets an entry under `## [Unreleased]` for user-visible changes.

## Versioning and artifacts

- The version lives in exactly one place: `qt_commander.__version__`. The
  release tag is `v<that version>`, and the C++ runtime must report the same
  number in its descriptor (`kLibraryVersion` in `src/library/server.cpp`).
- Never commit binaries: `artifacts/` and `build/` are generated. Release
  archives are produced by `.github/workflows/release.yml`.
- A new artifact layout or manifest field is a compatibility change: update
  `qt_commander/artifacts.py`, the docs, and the tests together.

## Code style

No linter is enforced yet; match the surrounding code (4-space indent, roughly
80 columns, `from __future__ import annotations` in Python modules). Keep
Windows-only and POSIX-only paths explicitly separated rather than patched with
platform checks in the middle of shared logic.
