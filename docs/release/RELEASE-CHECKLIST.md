# Release checklist

Everything a maintainer has to do by hand. Work through it top to bottom.

## 1. Licence — done (MIT, 2026-11-11)

`LICENSE` is MIT, `pyproject.toml` declares `license = "MIT"` with the MIT
classifier, and the anti-996 text lives on as a non-binding note in
`docs/community/anti-996.md`. `LICENSE-NOTES.md` keeps the reasoning and the
options that were rejected; no action needed before the release.

## 2. Repository settings

- `gh repo edit --description "..."` (keep the current one, it names the modes)
- Topics: `mcp`, `mcp-server`, `qt`, `qml`, `qwidget`, `ai-agent`, `llm`,
  `ui-automation`, `gui-testing`, `dll-injection`, `qt5`, `qt6`, `automation`,
  `claude`, `cursor` (already set)
- Homepage: point at the docs site or the demo video once either exists
- Enable Discussions; pin an issue that says "read QUICKSTART before filing"
- Upload a social preview image (1200×630)

## 3. PyPI

Publishing uses Trusted Publishing; no API token is stored in the repository.

1. Create the project on PyPI (or reserve the name) for `qt-commander-mcp`.
2. Add a trusted publisher: owner `tiefeiyu`, repository `qt-commander`,
   workflow `release.yml`, environment `pypi`.
3. Create the `pypi` environment in the repository settings, then set the
   repository variable `PYPI_PUBLISH=true`. Without that variable the release
   workflow builds the wheel and skips publishing (so a tag never fails on a
   missing publisher).
4. Dry run locally first. Without the release tag the build reports a dev
   version (`0.8.1.devN`, meaning N commits after the last tag); to preview the
   exact release numbers, tag the candidate commit locally, build, then delete
   the tag:

   ```bash
   git tag -a v0.9.0 -m "preview"
   uv build                       # wheel is 0.9.0
   git tag -d v0.9.0
   uv venv .release-check
   uv pip install --python .release-check/Scripts/python.exe dist/*.whl
   .release-check/Scripts/qt-commander.exe doctor --json
   ```

## 4. First CI run

The workflows have never executed. Expect to fix:

- Qt availability on the runners. Current state after the first run:
  - **Windows Qt 5.15.2 and 6.8.3: working** (MSVC via `ilammy/msvc-dev-cmd`);
  - **Linux Qt 6.8.3: works**; **Linux Qt 5.15.2 removed** — aqtinstall cannot
    resolve the 5.15.2 linux packages ("packages ['qt_base'] were not found").
    A Qt 5 linux runtime can still be built locally with
    `scripts/package_artifacts.py`; say so in the release notes instead of
    attaching a Linux Qt 5 archive.
- the artifact archive names produced by `scripts/package_artifacts.py`
  (`qt-commander-artifacts-<os>-<arch>-*.zip`) versus what
  `qt-commander install-artifacts` filters for.

Also worth fixing when convenient: `actions/checkout@v4` and
`astral-sh/setup-uv@v5` run on a deprecated Node.js 20 and are currently forced
onto Node.js 24 by GitHub; bump them before GitHub removes the fallback.

What the current workflow set actually covers (verified on PR #1):

| Job | What it proves |
|---|---|
| `python` × 4 | Windows/Linux × 3.10/3.13, 175 unit tests |
| `wheel` | sdist→wheel installs and `qt-commander doctor` runs |
| `cpp` × 3 | Qt 5.15.2/MSVC and Qt 6.8.3/MSVC on Windows, Qt 6.8.3/gcc on Linux |
| `cpp` linux | launch-time injection E2E (`LD_PRELOAD` → attach → click → screenshot) |
| `macos-linked` | Qt 6.8.3 arm64: linked E2E and launch-time injection E2E (`DYLD_INSERT_LIBRARIES`) |

macOS needs one workaround already in the job: Qt 6.8.3's
`FindWrapOpenGL.cmake` links `-framework AGL`, which the macOS 26 SDK removed;
the job patches that line out of the installed kit and becomes a no-op once the
kit moves to 6.8.4+.

The packaging path was validated locally on Windows with Qt 5.15.2 and MSVC
19.44: CMake build → `scripts/package_artifacts.py` → zip →
`install-artifacts --from-dir` (checksum verified) → `doctor` listed the
artifact → `artifacts.find()` matched a target for it. The release *publish*
steps (checksums job, GitHub release upload, PyPI) have still never run; use a
throwaway tag to exercise them.

Validate by running the release workflow on a throwaway tag, then installing on
a clean machine:

```bash
uvx --from qt-commander-mcp qt-commander doctor
uvx --from qt-commander-mcp qt-commander install-artifacts --list
```

## 5. Tag and release

The tag *is* the version: nothing in the tree needs editing for a release, and
CI derives both the wheel version and the compiled-in runtime version from it.

```bash
git switch master
git merge --no-ff feature/...
git tag -a v0.9.0 -m "v0.9.0: ..."   # see the annotated style of earlier tags
git push origin master v0.9.0
gh release create v0.9.0 --generate-notes
```

Sanity-check the tag before pushing: `git describe --tags --dirty` must print
`v0.9.0` (not `v0.9.0-1-g...`), and `uv build` must produce
`qt_commander_mcp-0.9.0-*`.

The release workflow attaches the wheel, the sdist, one
`qt-commander-artifacts-*.zip` per platform/Qt combination, and `SHA256SUMS`.
Check that the archives unpack to `<os>-<arch>/...` at the top level, because
`install-artifacts` rejects anything else.

## 6. Announce

- README first screen: install line, demo GIF, "what it can/cannot do"
- `docs/agent-eval.md` numbers into the release notes
- Post: Reddit r/QtFramework, Qt Forum, HN (Show HN), 少数派/V2EX for the
  Chinese audience; MCP registries and `awesome-mcp-servers`

## 7. After the first release

- Watch issue templates for the first week; every "cannot attach" report is a
  missing `doctor` check.
- Add the failing scenario to `docs/agent-eval.md` before fixing it.
