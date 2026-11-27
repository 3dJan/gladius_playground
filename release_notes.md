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
- **Design Agent prompt card**: the prompt field, the "Clear session" action, the
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
- **Compact layout for phones and small windows**: Gladius now detects a phone-sized or touch-driven surface (a small viewport area or a coarse pointer) and switches to a compact layout: the Design Agent panel takes the whole main space, the preview and graph start hidden, and the top bar carries one toggle per window that swaps the window owning the main space. Touch targets grow to a 44 px floor, shortcut hints disappear, keyboard-only chrome is hidden, and browser page scrolling / pinch-zoom is suppressed over the canvas. Two fingers drive camera pan and pinch dolly on the web build; one finger still orbits. The realtime raymarch preview defaults to off on a compact surface because phone-class GPUs cannot keep up - it can still be enabled per preview. Settings ▸ Appearance ▸ "Layout mode" forces Auto / Desktop / Compact, and `?layout=compact|desktop|auto` (web) or `GLADIUS_LAYOUT` (desktop) overrides it for a single run.

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

- **Design Agent conversation section**: the transcript is no longer a flat list of `[Agent] ...`
  lines at the window margin. Each turn gets a role chip on the rail column and its text on the same
  content column the phase artifacts above use, so it reads as a continuation of the workflow;
  consecutive turns of one speaker no longer repeat their role line, tool traces are muted and cut
  short, a long answer is truncated with an explicit "Show N earlier turns" row instead of scrolling
  away, clicking a cut-off turn expands the transcript, and the section stays visible with a hint
  before the first turn exists. Set `GLADIUS_AGENT_DEMO` (desktop) or `?agentDemo=1` (web) to review
  the panel with an example session and no AI provider configured.
- **Design Agent provider failures are now readable**: a failed provider call no longer surfaces a raw
  transport string or vendor JSON. Failures are classified into actionable cases - missing or invalid
  API key, model not found, rate limited, out of quota, unreachable endpoint, timeout, malformed or
  empty response, and provider not configured - each with a one line summary and what to do about it
  (for example "Add the API key in Settings and try again"). The original provider text is still kept
  in "Developer diagnostics" for debugging.
- **No OpenRouter provider or models by default**: Gladius no longer seeds an `openrouter_default`
  provider profile or fetches its model list on first run. Providers and their models are added
  explicitly in Settings, and an empty provider now shows which step is missing instead of failing
  silently. Sending a prompt without a usable provider or model is refused before the draft is
  consumed, with a link straight to the provider settings.
- **Unanswered agent questions stay visible**: when the agent answers with a question instead of
  running a tool, the reply is pinned in a card above the prompt instead of being buried in the
  collapsed activity log. The card offers Continue (answer for this step) and Autonomous (switch to
  autonomous mode and keep going), the status line reports that the agent is waiting for a reply, and
  the current workflow phase is marked as needing attention.
- **Autonomy mode is visible in the panel**: the footer status line now carries a chip showing the
  active mode - Ask frequently, Ask at major decisions, or Work autonomously. Clicking it opens the
  three modes, and the choice is saved with the rest of the Design Agent settings. The chip is a
  quiet bordered label in the regular text colour rather than accent-coloured text, so it reads as a
  setting and not as a warning next to the status line.
- **Signal colours are now used consistently**: red text means one thing only - something failed and
  needs a fix. Blocked actions ("cannot delete, this is referenced"), missing or degraded states
  ("no 3MF model loaded", "NanoVDB is currently degraded"), and input hints are amber instead, and
  what the app is doing right now stays blue. The theme accent is a selection mark (borders, bands,
  active nodes) and is no longer used to colour text, so the transcript role lines, the
  "Required"/"Optional" labels, the "Show N earlier turns" row and the send icon read as ordinary
  text. The palette and the rules live in one header (`src/ui/SignalColors.h`) that all views share
  instead of each hard coding its own red.
- Removed the "Start fresh" control from the Design Agent. Resetting the design is what New Document
  does, and clearing the conversation is what "Clear session" does, so the extra toggle (and its
  mid-run template reseed) only caused accidental loss of work.
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
