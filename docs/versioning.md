# Versioning

Two numbers exist on purpose. Do not merge them.

| Number | Where it lives | What it means | When it moves |
|---|---|---|---|
| **Release version** — `0.9.0` | `qt_commander.__version__`, wheel/sdist metadata, `qt-commander --version`, git tag `v0.9.0`, GitHub release title, PyPI | The product's public history | Every user-visible release, following SemVer |
| **Runtime library version** — `2.1.0` | `kLibraryVersion` in `src/library/server.cpp`; sent in the `hello` reply and written to the rendezvous descriptor as `library_version` | The capability level of the code that runs *inside the target* | The wire contract, the advertised `capabilities`, or target-side behaviour changes |

They are separate because they describe different things:

- The package is what you install and upgrade.
- The runtime is what lives inside a target process. It can be **older than the
  package** (a process that was injected days ago and has not been restarted)
  or newer (a linked build from `master`). `qt_status` and
  `qt_target_info` expose `library_version` so a client can feature-detect
  instead of assuming.

A package release does **not** imply a runtime bump. `0.9.0` ships runtime
`2.1.0`, unchanged: this release is packaging, CLI and diagnostics work only —
no C++ changed, so nothing about the in-target contract changed.

## Rules

1. The release version has exactly one source: `qt_commander.__version__`.
   `pyproject.toml` reads it dynamically, and the tag must be `v<version>`.
   `qt-commander --version`, the wheel metadata and the release title must all
   agree.
2. The runtime version is bumped by hand in `src/library/server.cpp`:
   patch for fixes, minor for new capabilities (also add them to the
   `capabilities` array in `src/library/agent.cpp`), major only for a breaking
   protocol change — and in that case the protocol major in the descriptor and
   `client.py` must move together.
3. Never derive one number from the other, in code or in docs. A comparison
   between `library_version` and the package version is always a bug.
4. `qt_commander/cli.py` reports both: `doctor` shows the package version, and
   `doctor --pid` shows the runtime version of the target it inspects.

## History note

Before `0.9.0` the numbering had drifted: the Python package said `2.0.0`, the
git tags said `v0.x` (up to `v0.8.0`), and the runtime said `2.1.0`. `0.9.0`
puts the package back on the tag line and leaves the runtime version alone.
