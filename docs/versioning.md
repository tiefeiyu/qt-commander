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
| tagged | `0.9.0` |
| 11 commits after `v0.8.0` | `0.8.1.dev11+g03c552a.d20261111` |

Both sides produce that dev string **identically**, because they run the same
command: `git describe --dirty --tags --long --abbrev=7 --match v*`
(`git_describe_command` in `pyproject.toml`, and the same invocation in
`CMakeLists.txt`). Neither marks a dirty tree — a release is built from a tag,
and setuptools-scm's `node-and-date` scheme does not encode dirtiness either —
so a modified checkout still reports the version of the tagged commit it is
based on. `qt-commander doctor --pid` shows the runtime version next to the
package version and they are directly comparable.

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
