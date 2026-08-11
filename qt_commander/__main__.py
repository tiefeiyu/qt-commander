"""Allow running as: python -m qt_commander (prefer the qt-commander-mcp
console script — `uv run --project <root> qt-commander-mcp` — which does not
depend on the working directory)."""
from qt_commander.server import main

main()
