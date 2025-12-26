# Copilot CLI MCP configs

This folder contains ready-to-use MCP configuration snippets for the **GitHub Copilot CLI**.

Copilot CLI stores MCP server config at:

- `~/.copilot/mcp-config.json` (or `$XDG_CONFIG_HOME/.copilot/mcp-config.json`)

You can also apply a config **for a single session** using:

- `copilot --additional-mcp-config @/path/to/json`

## Serena MCP server

- `mcp-config.serena.json` enables **all** Serena tools (`"tools": ["*"]`).
- `mcp-config.serena.readonly.json` enable only read-only / navigation tools.

These configs mirror the Serena launch command already used by VS Code in this repo (`.vscode/mcp.json`).
