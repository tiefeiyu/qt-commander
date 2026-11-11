"""The version comes from the git tag; no source file may hardcode it."""

from __future__ import annotations

import re
from importlib.metadata import PackageNotFoundError, version as dist_version
from pathlib import Path

import pytest

from qt_commander import __version__, cli

ROOT = Path(__file__).resolve().parents[2]
PEP440 = re.compile(r"^\d+(\.\d+)*((a|b|rc|\.post|\.dev)[0-9]+)?(\+[a-z0-9.]+)?$")


def test_package_version_matches_installed_metadata():
    try:
        installed = dist_version("qt-commander-mcp")
    except PackageNotFoundError:
        pytest.skip("qt-commander is not installed in this environment")
    assert __version__ == installed


def test_version_is_pep440():
    assert PEP440.match(__version__), __version__


def test_cli_version_flag_prints_the_package_version(capsys):
    with pytest.raises(SystemExit) as info:
        cli.main(["--version"])
    assert info.value.code == 0
    assert __version__ in capsys.readouterr().out


def test_cpp_version_comes_from_the_build():
    server = (ROOT / "src/library/server.cpp").read_text(encoding="utf-8")
    agent = (ROOT / "src/library/agent.cpp").read_text(encoding="utf-8")
    assert "kLibraryVersion = QT_COMMANDER_VERSION" in server

    lines = agent.splitlines()
    start = next(i for i, line in enumerate(lines) if "library_version" in line)
    snippet = "\n".join(lines[start:start + 2])
    assert "QT_COMMANDER_VERSION" in snippet
    assert not re.search(r'QStringLiteral\("\d+\.\d+', snippet)


def test_version_header_only_provides_a_fallback():
    header = (ROOT / "src/include/qt_commander/version.h").read_text(
        encoding="utf-8"
    )
    assert "#ifndef QT_COMMANDER_VERSION" in header
    assert "0.0.0+unknown" in header


def test_pyproject_takes_the_version_from_the_tag():
    text = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    assert "setuptools-scm" in text
    assert "[tool.setuptools_scm]" in text
    assert "tag_regex" in text
    assert "version = {attr" not in text
    assert '[tool.setuptools.dynamic]' not in text
