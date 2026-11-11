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

## 3. PyPI (one-time setup for automatic publishing)

Publishing uses **Trusted Publishing**: GitHub's OIDC identity is exchanged for
a short-lived token, so no API token and no secret ever lives in the repository.

1. Log in to PyPI as the project owner. 2FA is mandatory for uploading.
2. Register the publisher for the existing project:
   <https://pypi.org/manage/project/qt-commander-mcp/settings/publishing/>
   → **Add a new publisher**, and fill in exactly:
   - PyPI Project Name: `qt-commander-mcp`
   - Owner: `tiefeiyu`
   - Repository name: `qt-commander`
   - Workflow name: `release.yml`
   - Environment name: `pypi`
   (For a brand-new project the same form is at
   <https://pypi.org/manage/account/publishing/> as a *pending* publisher.)
3. Turn the switch on: `gh variable set PYPI_PUBLISH --body true`.
   The workflow job is also gated on `startsWith(github.ref, 'refs/tags/')`, so
   a manual run from a branch can never publish a dev build.
4. The `pypi` environment already exists in the repository (no protection
   rules, so a tag publishes without an approval prompt).
5. Verify after the next tag: <https://pypi.org/project/qt-commander-mcp/>

To rehearse without consuming a real version, tag a release candidate
(`v0.9.1rc1`): PyPI accepts rc versions and they sort below the final release.

Local dry run before tagging (no PyPI involved):

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
python scripts/release.py 0.9.1
```

The script refuses a dirty tree, a branch other than `master`, an existing tag
(local or remote) and unpushed commits; it runs the unit tests, creates the
annotated tag, pushes it, follows the `Release` workflow and prints the release
and PyPI URLs. Add `--dry-run` to stop before tagging, `--no-wait` to skip the
follow-up.

Manual equivalent:

```bash
git switch master
git tag -a v0.9.1 -m "v0.9.1: ..."   # see the annotated style of earlier tags
git push origin v0.9.1
```

Sanity-check before pushing: `git describe --tags --dirty` must print the tag
itself (not `v0.9.1-1-g...`), and `uv build` must produce
`qt_commander_mcp-0.9.1-*`.

The release workflow attaches the wheel, the sdist, one
`qt-commander-artifacts-*.zip` per platform/Qt combination, and `SHA256SUMS`.
Check that the archives unpack to `<os>-<arch>/...` at the top level, because
`install-artifacts` rejects anything else.

To repair the assets of an existing tag (a failed publish job, a build
problem), run the workflow manually with the tag input:

```bash
gh workflow run release.yml --ref master -f tag=v0.9.0
```

That path pins `SETUPTOOLS_SCM_PRETEND_VERSION` and `-DQTC_VERSION` to the tag
being republished, so it never produces dev-versioned artifacts, and the PyPI
job is skipped because the ref is a branch.

## 6. Announce

- README first screen: install line, demo GIF, "what it can/cannot do"
- `docs/agent-eval.md` numbers into the release notes
- Post: Reddit r/QtFramework, Qt Forum, HN (Show HN), 少数派/V2EX for the
  Chinese audience; MCP registries and `awesome-mcp-servers`

## 7. After the first release

- Watch issue templates for the first week; every "cannot attach" report is a
  missing `doctor` check.
- Add the failing scenario to `docs/agent-eval.md` before fixing it.
