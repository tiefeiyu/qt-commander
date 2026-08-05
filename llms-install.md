# qt-commander MCP Server — AI Agent Install Guide

Copy the prompt below and paste it into your AI agent to install qt-commander MCP Server.

---

> Please install qt-commander MCP Server — an MCP server for Qt application introspection and automation (widget tree inspection, property read/write, mouse/keyboard input, screenshot, etc.):
>
> 1. Install the Python package from the current directory:
>    ```bash
>    pip install -e .
>    ```
>    Requires Python 3.10+. Dependencies: `fastmcp`, `psutil`.
>
>    **uv (recommended — no global install needed):** run the server via
>    ```bash
>    uv run python -m qt_commander
>    ```
>    uv resolves `pyproject.toml` / `uv.lock` and manages an isolated
>    environment automatically (first run creates it, later runs are fast).
>
> 2. Add qt-commander to the MCP configuration for this platform.
>    The `command` below works with a normal `pip install -e .`; if you
>    use uv instead, replace `python` with `uv run python` (keep the
>    `cwd` pointing at the project root):
>
>    **Claude Code** — run:
>    ```bash
>    claude mcp add qt-commander -- python -m qt_commander
>    ```
>
>    **Claude Desktop** — edit `claude_desktop_config.json`:
>    - macOS:   `~/Library/Application Support/Claude/`
>    - Windows: `%APPDATA%\Claude\`
>    - Linux:   `~/.config/Claude/`
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "/absolute/path/to/qt-commander"
>        }
>      }
>    }
>    ```
>
>    **Cursor** — create/edit `.cursor/mcp.json` (project-root):
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "${workspaceFolder}"
>        }
>      }
>    }
>    ```
>
>    **Windsurf** — edit `~/.codeium/windsurf/mcp_config.json`:
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "/absolute/path/to/qt-commander"
>        }
>      }
>    }
>    ```
>
>    **VS Code / GitHub Copilot** — create/edit `.vscode/mcp.json` (note: uses `"servers"` key):
>    ```json
>    {
>      "servers": {
>        "qt-commander": {
>          "type": "stdio",
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "${workspaceFolder}"
>        }
>      }
>    }
>    ```
>
>    **Cline / Roo Code** — edit `cline_mcp_settings.json`:
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "/absolute/path/to/qt-commander"
>        }
>      }
>    }
>    ```
>
>    **Gemini CLI** — edit `~/.gemini/settings.json`:
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "python",
>          "args": ["-c", "from qt_commander.server import main; main()"],
>          "cwd": "/absolute/path/to/qt-commander"
>        }
>      }
>    }
>    ```
>
> 3. Restart the agent to load qt-commander. Then verify by calling `qt_list_processes` — it should return a list of running Qt processes (may be empty, but must not error).
>
> 4. (Optional) To use `qt_attach` and `qt_build`, Visual Studio 2022+ and Qt 5.15+ with MSVC are required. When building for the first time, pass the environment paths:
>    ```
>    qt_build(qt_env="C:/Qt/5.15.2/msvc2019_64/bin/qtenv2.bat",
>             vcvars_path="C:/.../vcvars64.bat", vcvars_args="amd64")
>    ```
>
> 5. If any step fails, read `README.md` or `README_zh.md` for manual setup instructions.
