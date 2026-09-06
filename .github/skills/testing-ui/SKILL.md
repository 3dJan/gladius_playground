---
name: testing-ui
description: Guide for autonomously testing the Gladius ImGui UI using MCP tools like ui_dump_windows, ui_dump_items, ui_click, and capture_screenshot. Covers interacting with the Dear ImGui Test Engine, bypassing the welcome screen, and preventing deadlocks.
---

# Testing UI in Gladius

Gladius uses **Dear ImGui** for its interface, integrated with the **Dear ImGui Test Engine**, which allows the MCP server to programmatically dump window layouts and simulate user inputs.

As an AI agent, you can autonomously modify the UI source code, compile the application, and test your changes end-to-end entirely through the MCP server.

## Core MCP UI Tools

- `ui_dump_windows`: Returns a list of all active top-level GUI windows.
- `ui_dump_items`: Supply a path (e.g., `"//Model Editor"`) to retrieve all interactable/registered UI elements within that scope.
- `ui_click`: Simulates a mouse click on an exact UI path.
- `capture_screenshot`: Captures the current visual state of the application. It returns a Base64 encoded image directly in the chat context, allowing you to visually verify layouts without file system juggling.

## Standard UI Testing Workflow

### 1. Application Initialization
When Gladius starts, it often loads a Welcome Screen. Before interacting with main application windows (like the Model Editor or MainMenu), you must either create a new document or open an existing one.
- **Option A (Tool)**: Call the `#tool:create_document` MCP tool.
- **Option B (UI Path)**: Simulate a UI click on the welcome screen button:
  `ui_click(path="//Welcome to Gladius/ActionsPane_CF70A5C3/New Project")`

Build the application with the configured **Build ALL (linux-releaseWithDebug)**
task. If you need to run a build command manually, use the project RTK wrapper
and keep diagnostic output compact (for example, `rtk err cmake ...`).

### 2. Discovering Windows
Once initialized, use `#tool:ui_dump_windows` to see what is currently visible.
Common windows include:
- `##MainMenuBar` (The top-level application menu)
- `MainWindowDockingArea` (The background dock space)
- `Model Editor` (The node graph/code view panel)
- `Preview` (The 3D render viewport)

*Note: Windows like `Model Editor` may only appear after a document is loaded or if manually toggled via the menu bar.*

### 3. Finding Interactive Elements
To interact with buttons, tabs, or inputs, you need their exact ImGui Test Engine path.
Use `#tool:ui_dump_items` and supply a specific window as the root (e.g., `//Model Editor`).

**⚠️ Warning:** Avoid dumping the global root path `""` if the engine is under heavy load, as dumping the entire nested context of the application can cause timeouts in the test engine bindings.

### 4. Executing Operations
Once you discover the path, use `#tool:ui_click`. Provide the *exact* string. 

**Common Known Paths:**
- Code Tab: `"//Model Editor/FunctionTabs/Code"`
- Graph Tab: `"//Model Editor/FunctionTabs/Graph"`
- Graph MenuBar Toggle: `"//##MainMenuBar/\uf542\tGraph"` (or the equivalent UTF-8 encode for `ICON_FA_PROJECT_DIAGRAM`)

### 5. Visual Verification
After executing your clicks, call `#tool:capture_screenshot`. The MCP server is capable of encoding the screenshot in Base64 directly into the JSON-RPC response, so you can literally "see" if your UI layout changes applied successfully.

## MCP Server Modes: Headless vs UI Mode

Two Gladius MCP server entries are configured in `.vscode/mcp.json`:

| Server name | Args | Use case |
|---|---|---|
| `gladius` | `--mcp-stdio --headless` | Default — no visible window, for modeling/generation tasks |
| `gladius-ui` | `--mcp-stdio` | UI debugging — shows a real Gladius window you can observe and interact with |

**Use `gladius-ui` for any session involving `ui_click`, `ui_dump_items`, or `capture_screenshot`** so you can visually verify what the agent is doing.

Both servers are managed by VS Code's MCP host. They restart automatically when killed.

## Restarting the MCP Server After a Rebuild

**Do NOT add a stop/restart MCP tool.** A process cannot reliably send a response after killing itself — the connection would be severed before the reply reaches the client.

The correct pattern is:

- **Headless server** (`gladius`): run `rtk proxy sh -c "pkill -f 'gladiusmcp --mcp-stdio --headless'"`, or use the VS Code task **"Restart Gladius MCP Server (headless)"**.
- **UI-mode server** (`gladius-ui`): run `rtk proxy sh -c "pkill -f 'gladiusmcp --mcp-stdio$'"`, or use the VS Code task **"Restart Gladius MCP Server (UI mode)"**.

Prefer the configured VS Code restart task. For manual process control, use
`rtk proxy sh -c "pkill ..."` when RTK tracking is useful, but do not use
`rtk err`, `rtk summary`, or other output filters around an MCP stdio server.
MCP protocol bytes must remain unmodified; never pipe the server's stdout
through a filtering wrapper.

VS Code detects the process exiting and **automatically relaunches** the server with the new binary on the next tool call. Wait ~2 seconds before issuing the next MCP call.

> **Why `pkill` works**: The MCP server entries in `.vscode/mcp.json` declare `"type": "stdio"`. VS Code's MCP host monitors each process and applies an automatic restart on unexpected exit.

## Common Pitfalls and Debugging Strategies

1. **Deadlocks and Timeouts:**
   The MCP wrapper around the ImGui Test Engine runs synchronously. If you command a `ui_click` on a path that **does not exist**, or is **obscured/disabled**, the test engine may block indefinitely waiting for the element to become interactable. 
   - *Fix*: Always verify the exact path using `ui_dump_items` before attempting to click.

2. **FontAwesome Icons in Paths:**
   ImGui labels often use FontAwesome macros. For example:
   ```cpp
   ImGui::Button(ICON_FA_PROJECT_DIAGRAM "\tGraph")
   ```
   In the UI string dumps, this translates to the literal unicode of the icon (`\uf542` or `\xef\x95\x82`) prefixed to the string. Do not assume the path is simply `"Graph"`. Read the dump carefully.

3. **Dynamic Hashes (`##` and `_ID`):**
   ImGui uses `##` to hide ID markers from the display text. A window titled `"Debug##Default"` will have the path name `"Debug##Default"`.
   Similarly, ImGui Test Engine sometimes appends hash suffixes for uniqueness (e.g., `ActionsPane_CF70A5C3`). Trust the dump output over reading the C++ raw strings.

4. **Window Focus / Tab States:**
   When testing code tabs vs graph tabs: ensure the `Model Editor` window is visible first. If your UI element is hidden behind a collapsed tab item or a closed window, `ui_click` won't be able to reach it.