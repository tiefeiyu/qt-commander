# Versioning

**The git tag is the version.** `v0.9.0` means version `0.9.0`. No file in this
repository contains the release version, and releasing is `git tag` — there is
nothing to bump.

## How each consumer gets it

| Consumer | When | How |
|---|---|---|
| Python package (wheel/sdist metadata, `qt-commander --version`, `doctor`) | build/install | `setuptools-scm` derives it from `git describe`; at runtime `qt_commander.__version__` reads the installed metadata via `importlib.metadata` |
| C++ runtime (`library_version` in `hello` and in the descriptor) | CMake configure | the top-level `CMakeLists.txt` runs `git describe` and passes `-DQT_COMMANDER_VERSION=...`; `src/include/qt_commander/version.h` is only a fallback |
| GitHub release title, PyPI version, CHANGELOG heading | release time | the tag |

So there is one place to change the version — the tag — and each build stage
reads it. They must agree, and CI inside a tag build guarantees it, because
both the wheel and the runtime are built from that same checkout.

## What the numbers look like

| Situation | Version |
|---|---|
| tagged, clean tree | `0.9.0` |
| tagged, modified tree | `0.9.1.dev0` |
| 13 commits after `v0.8.0` | `0.8.1.dev13` |

Both sides produce those strings **identically**: `guess-next-dev` with no
local segment. The version deliberately carries no commit hash and no date —
those make the string depend on *how* the build was invoked (setuptools-scm
changes its local part between `pip install .` and `python -m build`), which
would defeat the point of having one number. Commit identity belongs to the tag
for releases and to `git describe` while developing; when you need to know
whether a *running* target is stale, `qt_target_info.loaded_injector.stale`
compares the library's mtime with the process start time, which is more reliable
than comparing version strings.

A modified tree is never treated as the release: tagging a dirty checkout yields
`X.Y.(Z+1).dev0`, on both sides, so a release can only come from a clean tag.

`qt-commander doctor --pid` shows the runtime version next to the package
version and they are directly comparable.

## Commit identity lives in the artifact, not the version

The version says *which release*, not *which commit* — that is deliberate, so it
stays reproducible. An artifact records the commit separately:

```json
// <build>/qtc-build-info.json, written by CMake at configure time
{
  "version": "0.8.1.dev14",
  "commit": "3760124f514852d19880060f35e7088c5c37c2e0",
  "describe": "v0.8.0-14-g3760124"
}
```

`qt_build` and `scripts/package_artifacts.py` copy that file into the artifact's
`manifest.json` as the `source` block, and `qt_target_info` /
`qt_commander doctor --pid` print it (`built from 0.8.1.dev14, 3760124f`). So a
released artifact is traceable all the way back to a commit, and a development
artifact is distinguishable from another one built at the same distance from the
tag. Manifests produced before this existed simply have no `source` block.

## Releasing

```bash
git switch master
git merge --no-ff feature/...
git tag -a v0.9.0 -m "v0.9.0: ..."
git push origin master v0.9.0
```

CI then builds the wheel and the artifact archives from that checkout, and
publishes both with version `0.9.0`. No file is edited for a release, which also
means a release cannot forget to bump something.

## Fallbacks (no `.git`)

- **Python from an sdist**: the sdist records the version in its metadata, so a
  wheel built from it keeps the released version. A source tree with no VCS and
  no metadata reports `0.0.0+unknown`.
- **CMake from a tarball**: pass `-DQTC_VERSION=0.9.0` explicitly (that is what
  a CI tarball build would do); otherwise the runtime reports `0.0.0+unknown`.
- CI checkouts must use `fetch-depth: 0`, or `git describe` sees no tags and the
  build silently falls back. The workflows in this repository already do.

## The runtime version is a snapshot, not a live value

The runtime is compiled into the target process; it cannot run `git describe`
later. Its `library_version` therefore says which release the *injected code*
came from, which is exactly what you want when a process has been alive for days
or was injected before an upgrade. It is normal for a running target to report
an older version than the installed package — the difference means "restart the
target to pick up the new runtime".

Wire compatibility is not tracked by this version: the descriptor's
`protocol.major` (`2`) is the compatibility discriminator, and `capabilities`
advertises optional features. A client should check those, not compare versions.

## History

- Until `v0.8.0`: three numbers drifted apart — the package said `2.0.0`, the
  tags said `v0.x`, and the runtime reported `2.1.0` (with the descriptor
  disagreeing with `hello`, `2.0.0` vs `2.1.0`, because both were hardcoded).
- `v0.9.0` onwards: the tag is the only source, the package and the runtime both
  derive from it, and CI builds from the tag.
