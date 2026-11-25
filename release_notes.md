# Gladius Release Notes

## Version 1.2.14 (Unreleased)

### Features

- **Design Agent workflow panel**: the Design Agent now lives in one dockable side panel instead of
  three separate windows. A vertical workflow tree shows the five design phases (Understand,
  Decompose, Explore, Develop, Validate) as circles on one continuous line, and each phase expands
  in place to show the artifacts it actually produced: requirements, the design structure,
  alternatives and system configurations with real previews, planning tasks, and validation.
  Measured evaluator results are shown separately from unverified agent claims, the prompt and agent
  status stay pinned at the bottom of the panel, and the former verbose logs moved into collapsible
  "Activity log" / "Developer diagnostics" sections. A run can now be paused and resumed between
  agent steps.
- **Design Agent prompt card**: the prompt field, the "Start fresh" / "Clear session" actions, the
  model and fallback pickers and the send action now sit inside a single rounded card with a
  focus-highlighted border, the way modern chat composers look. An empty field shows the
  placeholder "What do you want to build?", send is an icon button, "Attach image" and
  "Paste image" are icon buttons in the card's toolbar, and the pickers stay usable at half the
  previous panel width by wrapping onto a second toolbar row instead of shrinking. The status line
  and the pause / stop / settings controls stay above the card, and the footer height is measured
  from its contents instead of being hard coded.
- **Large Design Agent prompts**: the Design Agent prompt field is no longer limited to ~2000
  characters and two visible lines. It now grows with the text up to ten lines, `Ctrl/Cmd+Enter`
  sends (plain `Enter` inserts a newline), a maximize button opens a resizable
  "Agent Prompt Editor" window bound to the same draft, and dropping a `.txt`/`.md` file onto the
  window appends its contents as prompt instructions (desktop only).
- **Smooth window resize experience**: Eliminated flicker and clearing of rendered content during window resize operations. The viewport now preserves the displayed 3D scene during manual resize, maximize/restore, and multi-monitor transitions, providing a more professional and polished user experience.

### Improvements

- Added minimum viewport dimension validation to prevent edge cases with very small window sizes
- MCP server over HTTP is now documented and wired up for agents: `.mcp.json` exposes a `gladius`
  `type: "http"` entry alongside the `gladius-stdio` fallback, VS Code tasks start the HTTP server
  (UI or headless), the protocol docs describe the real routes (`POST /`, `GET /health`) and method
  allow-list, and new HTTP transport tests cover the routing, CORS preflight and stop semantics

## Version 1.2.13

### Features

- Improved slice view
  - Automatic view adjustments
  - Rulers

### Bug Fixes

- Fixing issues in the app initialization
