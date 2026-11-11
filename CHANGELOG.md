# Changelog

Notable changes are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

The version is the git tag: `setuptools-scm` derives the package version from it
at build time, and CMake compiles the same tag into the C++ runtime (see
[docs/versioning.md](docs/versioning.md)). Nothing in the tree hardcodes it, so
a heading here always matches a tag after the release.

Before 0.9.0 the project was installed from source only; the `v0.x` git tags
below are pre-PyPI milestones and are kept for history.

## [0.9.1] — 2026-11-11

### Added

- `scripts/release.py`: one command to cut a release (dirty/branch/tag checks,
  unit tests, annotated tag, push, follow the workflow).
- PyPI publishing through Trusted Publishing (OIDC): the `Release` workflow's
  `pypi` job publishes tag pushes without any API token in the repository.

### Fixed

- The MCP server refused to start on any machine that routes traffic through a
  SOCKS proxy. FastMCP's startup banner asks PyPI for a newer version, and httpx
  raises `ImportError: Using SOCKS proxy, but the 'socksio' package is not
  installed` while constructing the client, which killed the server before it
  served anything. The update check is now off by default — startup touches no
  network — and `FASTMCP_CHECK_FOR_UPDATES=stable` turns it back on.
- The MSVC compiler probe always reported "unknown", so Windows artifacts were
  named `msvc-unknown` and their manifests carried no compiler version. `cl`
  prints its usage on stdout and the (localized) version banner on stderr, and
  the banner can contain bytes that are invalid in the console code page; the
  old single-stream `text=True` read lost it. Both streams are read and decoded
  leniently.
- Pre-release tags were compiled into the runtime as the raw `git describe`
  string instead of the tag version. Tagged builds now report the tag itself; a
  modified tree appends `.dev0`.
- The release workflow could not create the GitHub release at all (the job has
  no checkout, so `gh` could not infer the repository), and a manual
  `workflow_dispatch` run rebuilt artifacts with a dev version. `GH_REPO`, an
  optional `tag` input, `SETUPTOOLS_SCM_PRETEND_VERSION` and a quoted
  `-DQTC_VERSION` fix both; the `pypi` job is restricted to tag refs so a manual
  run can never publish a dev build.
- Release candidates are now marked as pre-releases, so the GitHub "Latest"
  badge stays on the stable version.

## [0.9.0] — 2026-11-11

### Added

- `qt_launch(program, library=...)`: start an application with the runtime
  preloaded (`LD_PRELOAD` on Linux, `DYLD_INSERT_LIBRARIES` on macOS), wait for
  its descriptor and attach. Accepts a macOS `.app` bundle. Not registered on
  Windows, which has no preload hook.
- `qt-commander` command line: `serve`, `doctor`, `install-artifacts`.
- Prebuilt injector archives per release, installed and checksum-verified by
  `qt-commander install-artifacts` instead of compiling the C++ runtime by hand.
- `scripts/package_artifacts.py` packages a plain CMake build tree into the
  artifact layout (used by the release workflow).
- `QT_COMMANDER_ARTIFACTS` selects the artifact root, so an installed wheel can
  keep artifacts in a per-user directory.
- Artifact manifests record a `source` block (`version`, `commit`, `describe`)
  taken from the CMake configure step, so an artifact is traceable to a commit
  even when the version alone cannot distinguish dev builds. `qt_target_info`
  and `doctor --pid` print it.

### Changed

- `qt_build` default build directory now follows the resolved source tree.
- One version everywhere: the git tag is the only source. `setuptools-scm`
  derives the package version from it, and CMake compiles it into the runtime,
  so `library_version` is no longer a second, drifting number (it was `2.1.0`
  in `hello` and `2.0.0` in the descriptor, while the package said `2.0.0` and
  the tags said `v0.8.0`).
- The licence is now **MIT**. It was `MIT AND Anti-996` up to v0.8.0, which PyPI
  reports as "Other" and many legal reviews cannot accept; previous releases
  keep their original terms. The anti-996 text is kept as a non-binding note.
- Package metadata carries readme, keywords, classifiers and project URLs.

### Fixed

- `src/library/agent.cpp` used `QFile::readLink()`, which exists in neither Qt 5
  nor Qt 6, so the POSIX scope probe never compiled on Linux or macOS. It now
  uses `QFile::symLinkTarget()`. CI caught this on its first real Linux build.
- Version/commit resolution moved from the top-level `CMakeLists.txt` into
  `src/CMakeLists.txt`: the linked test apps add `src/` to their own project,
  so they were compiling an empty version and the runtime reported `""`.
- The inject library's startup no longer polls for 10 s before giving up: it is
  started from a `Q_COREAPP_STARTUP_FUNCTION` when the loader preloads it, with
  the polling path kept for runtime injection into an already-running process.
- A Release target could be reported as `debug: true` when the loader `PATH`
  contained a full Qt kit `bin`, so a Debug injector was loaded and timed out
  waiting for `QCoreApplication`. The injected runtime's flavour now comes from
  the modules actually loaded into the process; a directory holding both Qt
  runtimes reports `debug: null` and artifact selection asks instead of guessing.

## [0.8.0] — 2026-11-10

### Added

- `qt_target_info` reports `loaded_injector`, `expected_artifact`,
  `injection_state`, `restart_required` and `detected_from`.
- `qt_attach` failures carry `error.reason`, `error.restart_required` and
  `error.details` (target snapshot, chosen artifact, loaded injector, expected
  artifact). An already mapped library is refused before injecting.
- `qt_build(for_pid=...)` derives Qt major version, toolchain, architecture,
  build type, Qt kit and `vcvars_path` from the target; `source_dir` is
  optional.
- `manifest.json` (schema 2) records the canonical `build_id`.

### Fixed

- Injection failure was reported after a 20 s wait with no diagnosis; the
  library status file is now polled, so the reason arrives in about 10 s.

## [0.7.0] — 2026-11-09

### Added

- Async task results: `qt_act`/`qt_call`/`qt_set` return
  `{"state": "running", "task_id": ...}` when the target is still inside the
  call, and `qt_task` collects the result. The queue is not blocked meanwhile.

## [0.6.1] — 2026-11-09

### Fixed

- `qt_batch` shares one deadline across steps; an exhausted budget returns the
  completed steps with `cause="batch_timeout"` instead of losing them.

## [0.6.0] — 2026-11-09

### Added

- Bounded source scans, background build jobs with `qt_build_status`, and
  module-based target discovery for processes without a descriptor.

## [0.5.1] — 2026-11-06

### Fixed

- QML click target preservation and toolchain detection from executable imports.

## [0.5.0] — 2026-11-06

### Added

- One nested, target-keyed artifact root instead of per-directory manifests.

## [0.4.0] — 2026-11-04

### Added

- `qt_processes` discovers running Qt targets without injecting.

## [0.3.2] — 2026-11-04

### Documentation

- Installation guide for linked and injection modes.
