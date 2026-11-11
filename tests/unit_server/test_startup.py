"""The MCP server must boot without network access.

FastMCP prints a banner on startup and, unless told otherwise, asks PyPI whether
a newer version exists. That request goes through httpx, and httpx raises at
*client construction* when a SOCKS proxy is configured without ``socksio``:

    ImportError: Using SOCKS proxy, but the 'socksio' package is not installed.

The exception is not caught by FastMCP's version check, so the server died
before serving anything. These tests pin the workaround: no update check, so no
httpx client, so no dependency on the network or on proxy support.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _run(code: str, **env: str) -> str:
    environment = {
        key: value for key, value in os.environ.items()
        if key != "FASTMCP_CHECK_FOR_UPDATES"
    }
    environment.update(env)
    completed = subprocess.run(
        [sys.executable, "-c", code],
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr
    return completed.stdout.strip()


def test_the_update_check_is_disabled_by_default():
    output = _run(
        "import qt_commander.server, fastmcp;"
        " print(fastmcp.settings.check_for_updates)"
    )
    assert output == "off"


def test_no_network_client_is_created_at_startup():
    code = (
        "import httpx2\n"
        "def boom(*args, **kwargs):\n"
        "    raise AssertionError('network access attempted at startup')\n"
        "httpx2.get = boom\n"
        "import qt_commander.server  # noqa: F401\n"
        "from fastmcp.utilities.version_check import check_for_newer_version\n"
        "print(check_for_newer_version())"
    )
    assert _run(code) == "None"


def test_the_default_can_be_overridden():
    output = _run(
        "import qt_commander.server, fastmcp;"
        " print(fastmcp.settings.check_for_updates)",
        FASTMCP_CHECK_FOR_UPDATES="stable",
    )
    assert output == "stable"
