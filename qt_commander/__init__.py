"""qt-commander: in-process Qt UI automation over a JSON-RPC connection."""

from importlib.metadata import PackageNotFoundError, version as _dist_version

# The release version comes from the git tag through setuptools-scm; it is
# baked into the installed package metadata at build time, so nothing in this
# repository hardcodes it. A source tree that was never installed has no
# metadata, hence the fallback. See docs/versioning.md.
try:
    __version__ = _dist_version("qt-commander-mcp")
except PackageNotFoundError:  # pragma: no cover - exercised by uninstalled use
    __version__ = "0.0.0+unknown"
