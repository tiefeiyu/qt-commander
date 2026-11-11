# Changelog

Notable changes are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

One version number describes both halves: the Python package version equals the
`library_version` the C++ runtime reports in its rendezvous descriptor.

Before 2.1.0 the project was installed from source only; the `v0.x` git tags
below are pre-PyPI milestones and are kept for history.

## [Unreleased]

### Added

- `qt-commander` command line: `serve`, `doctor`, `install-artifacts`.
- Prebuilt injector archives per release, installed and checksum-verified by
  `qt-commander install-artifacts` instead of compiling the C++ runtime by hand.
- `scripts/package_artifacts.py` packages a plain CMake build tree into the
  artifact layout (used by the release workflow).
- `QT_COMMANDER_ARTIFACTS` selects the artifact root, so an installed wheel can
  keep artifacts in a per-user directory.

### Changed

- `qt_build` default build directory now follows the resolved source tree.
- Version is read from `qt_commander.__version__` (2.1.0); the wheel, runtime
  and release tag share one number.
- Package metadata carries readme, keywords, classifiers and project URLs, and
  declares the actual `LICENSE` file instead of a mismatched "MIT" string.

### Fixed

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
