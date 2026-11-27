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

- **Design Agent failure reporting**: an agent run that ended without a result is now explained
  instead of silently going quiet. A model turn that returned neither text nor a tool call (a local
  endpoint spending its whole output budget on hidden reasoning, for example) used to be treated as
  "the agent is done": the run reported success with an empty document and no message. The loop now
  retries such a turn twice and then reports a classified error in the status line, pointing at the
  setting that fixes it. Turns cut off at the model output limit get the split-your-snippets hint
  even when no tool call followed them.
- **Design Agent per-request tunables**: the settings section now exposes **Max output tokens**,
  **Thinking tokens** (hidden-reasoning budget), **Request timeout** and **Prompt cache** breakpoints
  (Auto / None / Tools / All), persisted with the other agent settings and applied to every provider
  profile. The interactive defaults were raised to 8192 output tokens, a 1024 token thinking cap and a
  600 s request timeout — matching what the design-agent benchmark runs with — replacing the old
  4096 / uncapped reasoning / 120 s combination that produced truncated and empty answers on local models.

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
