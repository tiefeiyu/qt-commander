# Release checklist

Everything a maintainer has to do by hand. Work through it top to bottom; the
first two items block every later one.

## 1. Licence decision (blocking)

`LICENSE` is currently "MIT AND Anti-996", which PyPI reports as `Other` and
which some companies refuse by policy. Read `LICENSE-NOTES.md`, pick an option,
and update `LICENSE`, `pyproject.toml` and the README together.

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
4. Dry run locally first:

   ```bash
   uv build
   uv venv .release-check
   uv pip install --python .release-check/Scripts/python.exe dist/*.whl
   .release-check/Scripts/qt-commander.exe doctor --json
   ```

## 4. First CI run

The workflows have never executed. Expect to fix:

- Qt availability on the runners (`jurplel/install-qt-action` versions must be
  pinned to the exact kits: 5.15.2 and 6.8.3);
- MSVC environment (`ilammy/msvc-dev-cmd`) on `windows-latest`;
- the artifact archive names produced by `scripts/package_artifacts.py`
  (`qt-commander-artifacts-<os>-<arch>-*.zip`) versus what
  `qt-commander install-artifacts` filters for.

Validate by running the release workflow on a throwaway tag, then installing on
a clean machine:

```bash
uvx --from qt-commander-mcp qt-commander doctor
uvx --from qt-commander-mcp qt-commander install-artifacts --list
```

## 5. Tag and release

```bash
git switch master
git merge --no-ff feature/p0-distribution
git tag -a v2.1.0 -F tag-message.txt
git push origin master v2.1.0
gh release create v2.1.0 --generate-notes
```

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
