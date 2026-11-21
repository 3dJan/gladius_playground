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

Gladius MCP entries live in `.mcp.json` at the repository root:

| Server name | Transport | Mode | Use case |
|---|---|---|---|
| `gladius` | `http` (`http://localhost:8080/`) | UI, unless started with `--headless` | Default — collaborate over HTTP while a Gladius window shows the changes |
| `gladius-stdio` | `stdio` (`--mcp-stdio --headless`) | Headless | Fallback when no HTTP server is running |

**UI-only tools** (`ui_click`, `ui_dump_windows`, `ui_dump_items`, `capture_screenshot`) exist
only when the server runs without `--headless`. Before using them, confirm the mode:

```bash
curl http://localhost:8080/health          # server is up (use localhost, not 127.0.0.1)
```

then call `get_status` and check `headless: false`. If it reports `headless: true`, no UI tool
will work — start the server through the **"Start Gladius MCP Server (HTTP)"** task instead.

A `type: "http"` entry is **never launched by the client**; you start the server yourself with
one of the VS Code tasks:

- **"Start Gladius MCP Server (HTTP)"** — UI mode: Gladius window plus MCP server on port 8080
- **"Start Gladius MCP Server (HTTP, headless)"** — background server, no window

A UI-mode server exits as soon as its window is closed. Only the `gladius-stdio` entry is
managed by VS Code's MCP host and relaunched automatically.

## Restarting the MCP Server After a Rebuild

**Do NOT add a stop/restart MCP tool.** A process cannot reliably send a response after killing itself — the connection would be severed before the reply reaches the client.

The correct pattern is:

- **HTTP server**: use the VS Code task **"Start Gladius MCP Server (HTTP)"** or **"Start Gladius MCP Server (HTTP, headless)"** — both force-kill the old process first. Manually: `rtk proxy sh -c "pkill -9 -f 'gladiusmcp --mcp-server'"`, then run the new binary (or run the task).
- **stdio server** (`gladius-stdio`): run `rtk proxy sh -c "pkill -9 -f 'gladiusmcp --mcp-stdio'"`, or use the VS Code task **"Restart Gladius MCP Server (headless)"**. Match on `--mcp-stdio` so you do not also kill a running HTTP server.

Prefer the configured VS Code task. For manual process control, use
`rtk proxy sh -c "pkill ..."` when RTK tracking is useful, but do not use
`rtk err`, `rtk summary`, or other output filters around an MCP stdio server.
MCP protocol bytes must remain unmodified; never pipe the server's stdout
through a filtering wrapper.

The server ignores `SIGTERM`, so `pkill` alone does not stop it — always use `-9`. After
killing, wait ~2 seconds before issuing the next MCP call.

> **Why only stdio auto-restarts**: VS Code's MCP host monitors processes it launched itself,
> i.e. entries with `"type": "stdio"`. An `"type": "http"` entry points at a server you own,
> so after `pkill -9` you must start the new binary yourself.

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