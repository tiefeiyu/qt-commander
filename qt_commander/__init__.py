"""qt-commander: in-process Qt UI automation over a JSON-RPC connection."""

# Release version: single source for pyproject.toml, the git tag and PyPI.
# The in-target runtime reports its own `library_version` (kLibraryVersion in
# src/library/server.cpp), which moves independently; see docs/versioning.md.
__version__ = "0.9.0"
