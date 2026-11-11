# Roadmap

The goal is not "another Qt test framework". It is the most reliable way for an
agent to operate a **running, already-built** Qt application — including
closed-source C++ ones — and to be able to explain what went wrong when it
cannot.

Two properties are worth protecting because they are hard to copy:

- **Two delivery modes, one contract.** `linked` (instrument the app in CI) and
  `injection` (operate a deployed binary) expose exactly the same tools.
- **An agent-grade contract.** Typed failures with hints, transactional actions
  that survive modal dialogs, locator resolution inside the target (no TOCTOU),
  and budgeted responses.

## P0 — installable and trustworthy

Status: in progress on `feature/p0-distribution`.

- [x] `qt-commander` CLI: `serve`, `doctor`, `install-artifacts`
- [x] Prebuilt artifact archives with `SHA256SUMS`, installed without a compiler
- [x] Artifact root configurable (`QT_COMMANDER_ARTIFACTS`) so a wheel install
      works outside a checkout
- [x] One version number across package, runtime and tag
- [x] `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, issue/PR templates
- [ ] CI: Python matrix (Windows/Linux, 3.10–3.13) + C++ build + packaging smoke
- [ ] Release workflow: wheel + per-platform artifact archives on `v*` tags
- [ ] Publish to PyPI (`qt-commander-mcp`) and verify `uvx`/`pip install` on a
      clean machine
- [ ] 60-second demo: a bundled app, `qt-commander demo`, and a recording in the
      README
- [ ] Decide the licence (see `docs/release/LICENSE-NOTES.md`)

Acceptance: a new user on a clean Windows or Ubuntu machine reaches "agent
attached and read the tree" in under five minutes, without installing a C++
compiler.

## P1 — evidence that it works

- [ ] Agent benchmark: 30–50 tasks × 3 apps (QWidget, QML, large table) × 2–3
      models; publish success rate, tool calls, tokens and wall clock, against a
      screenshot-only baseline
- [ ] `qt_commander doctor` grows the checks the benchmark exposes
- [ ] MCP `resources` (live hierarchy snapshot, app/artifact metadata) and
      `prompts` (explore / verify / triage) so common reads cost no tool call
- [ ] Tool profiles to cut context cost (`QT_COMMANDER_TOOLSET=core|full`)
- [ ] Record/replay of an accepted interaction, plus a `pytest` plugin that
      emits JUnit XML — the bridge into QA budgets
- [x] macOS: linked mode in CI, plus launch-time injection (`qt_launch`,
      `DYLD_INSERT_LIBRARIES`). Attach-to-a-running-process is not possible
      there by OS design and is not planned.
- [ ] Screen-level screenshots on macOS: preflight the Screen Recording
      permission and return a structured error instead of a blank/crashing grab
- [ ] Parallel sessions: several targets per server, one rendezvous per worker

Acceptance: a published comparison table and a reproducible benchmark script.

## P2 — adoption

- [ ] Honest comparison page (Squish, qtPilot, pyqt-runtime-mcp, screenshot
      agents) including "when not to use this"
- [ ] Vertical case studies: Windows + MSVC + Qt 5.15 closed-source industrial
      software (the RealEvo / Qt Creator / QGC work already done)
- [ ] Docs site (mkdocs) in English and Chinese; recipes section
- [ ] Listed in the MCP registries and `awesome-mcp-servers`; a Claude Code
      skill and a Cursor rule shipped in-repo
- [ ] Talk + long-form write-up: "why injection beats screenshot agents for Qt"

## Investigated and deferred

- **One artifact per Qt major, built against the oldest supported version.**
  Qt promises binary compatibility forward within a major version, so a Qt
  5.12-built injector should serve 5.12-5.15 targets, and a 6.2-built one
  6.2-6.8. Measured in CI (Qt 5.12.12 injector into a Qt 5.15.2 app): the
  runtime does not compile against 5.12 without changes — it uses
  `QWidget::screen()` (5.14), `QTabBar::isTabVisible()` (5.15) and
  `Qt::SkipEmptyParts` (5.14) — and Qt 5.12 headers need `-include limits` with
  GCC 13+. Old Qt 6 kits are also awkward to provision: aqtinstall cannot
  resolve the default archives for 6.2.4 or 6.5.3 (`qt_base` naming), so a
  baseline pipeline would need explicit `--archives` or pinned images.
  Deferred until a user actually runs a target older than the shipped
  artifacts; until then `artifact_missing` points at `qt_build(for_pid=...)`.
- **Attaching to an already-running process on macOS.** Not possible: there is
  no `CreateRemoteThread` equivalent, and `task_for_pid` is refused for hardened
  or notarized builds. `qt_launch` (launch-time insertion) and linked mode cover
  the platform instead.

## Non-goals

- Becoming a GUI test framework with its own assertions, reporting and device
  farm. Pair with `pytest` instead.
- Operating non-Qt applications. Screenshot/computer-use agents are the right
  tool there.
- Bypassing security controls, attaching without authorization, or hiding the
  injector from the operating system.
