# qt-commander MCP Server — AI Agent Install Guide

Copy the prompt below and paste it into your AI agent to install qt-commander MCP Server.

---

> Please install qt-commander MCP Server — an MCP server for Qt application introspection and automation (widget tree inspection, property read/write, mouse/keyboard input, screenshot, etc.):
>
> 1. Run the server with **uv** (no global Python install needed — uv resolves
>    `pyproject.toml` / `uv.lock` and manages an isolated environment):
>    ```bash
>    uv run python -m qt_commander
>    ```
>    Requires uv (https://docs.astral.sh/uv) and Python 3.10+.
>
> 2. Add qt-commander to the MCP configuration for this platform. The
>    `command` is `uv` with `run python -m qt_commander`, and `cwd` must
>    point at the qt-commander project root:
>
>    **Claude Code** — run:
>    ```bash
>    claude mcp add qt-commander -- uv run python -m qt_commander
>    ```
>    (or add it manually to the Claude Code MCP config with the
>    `cwd` set to the project root)
>
>    **Claude Desktop** — edit `claude_desktop_config.json`:
>    - macOS:   `~/Library/Application Support/Claude/`
>    - Windows: `%APPDATA%\Claude\`
>    - Linux:   `~/.config/Claude/`
>    ```json
>    {
>      "mcpServers": {
>        "qt-commander": {
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
>          "command": "uv",
>          "args": ["run", "python", "-m", "qt_commander"],
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
