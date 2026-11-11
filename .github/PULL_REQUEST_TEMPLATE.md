## What this changes

<!-- One or two sentences. Link the issue if there is one. -->

## Why

<!-- The behaviour or bug that motivated it. "Because it is cleaner" is fine if true. -->

## Checklist

- [ ] `uv run pytest -q` passes locally
- [ ] New behaviour has a test (unit test, or an E2E script entry if it needs a real target)
- [ ] Tool descriptions / `INSTRUCTIONS` updated if the agent-facing contract changed
- [ ] `CHANGELOG.md` has an entry under `## [Unreleased]` for user-visible changes
- [ ] Docs updated (README / docs/design.md / ROADMAP.md) where behaviour is described
- [ ] No binaries or build output committed (`artifacts/`, `build/`, `dist/`)
- [ ] If a tool response grew, it is still budgeted (limits, `compact`, truncation signals)

## Notes for the reviewer

<!-- Anything subtle: Windows/Linux differences, C++ changes, compatibility of artifact/manifest fields. -->
