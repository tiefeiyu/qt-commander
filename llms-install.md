# qt-commander MCP Server — AI Agent Install Guide

Copy the prompt below and paste it into your AI agent to install qt-commander MCP Server.

---

> Please install qt-commander MCP Server — an MCP server for Qt application introspection and automation (widget tree inspection, property read/write, mouse/keyboard input, screenshot, etc.):
>
> 1. Run the server with **uv** (no global Python install needed — uv resolves
>    `pyproject.toml` / `uv.lock` and manages an isolated environment; the
>    first run auto-syncs the project venv and installs the `qt-commander-mcp`
>    console script):
>    ```bash
>    uv run --project /absolute/path/to/qt-commander qt-commander-mcp
>    ```
>    Requires uv (https://docs.astral.sh/uv) and Python 3.10+.
>
> 2. Add qt-commander to the MCP configuration for this platform. The
>    `command` is `uv` with `run --project <path/to/qt-commander>
>    qt-commander-mcp`. The `--project` flag pins the project location, so
>    **no `cwd` is needed** — the server starts from any working directory
>    (a `cwd`-dependent launch is a common cause of "MCP connection failed"):
>
>    **Claude Code** — run:
>    ```bash
>    claude mcp add qt-commander -- uv run --project /absolute/path/to/qt-commander qt-commander-mcp
>    ```
>    (or add it manually to the Claude Code MCP config)
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
>          "args": ["run", "--project", "/absolute/path/to/qt-commander", "qt-commander-mcp"]
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
