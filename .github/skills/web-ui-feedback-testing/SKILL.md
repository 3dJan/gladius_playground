---
name: web-ui-feedback-testing
description: >-
  Gather visual UX feedback and control or test the Gladius WebAssembly WebGPU
  UI in the integrated browser. Use for browser screenshots, visual QA, canvas
  interactions, reproducing web UI flows, or evaluating UI changes; this is
  separate from the desktop ImGui MCP testing skill.
metadata:
  author: gladius
  version: "1.0"
---

# Web UI Feedback and Testing in Gladius

Use this skill for the browser/WebAssembly build. The native desktop UI workflow
is documented separately in `.github/skills/testing-ui/SKILL.md`.

## Know what is being tested

Gladius is a C++/ImGui application compiled with Emscripten for WebGPU. Its
application UI is drawn into the `#gladius-canvas` canvas; it is not a
DOM-based web application. The HTML shell adds only a few real DOM status
messages. Therefore:

- Browser accessibility snapshots and DOM locators do **not** expose most
  Gladius buttons, menus, graph nodes, or panels.
- Use screenshots to understand the visible UI, then use pointer coordinates
  on the canvas for ImGui controls.
- `read_page` is still useful for page title, accessible DOM, and browser console
  events, but an unchanged or sparse snapshot does not mean the application is
  unresponsive.
- The browser build requires WebGPU and a compatible adapter. Native file
  dialogs, persistent settings, MCP, and other desktop-only features are not
  available; see `docs/building-web.md` before treating those differences as UI
  regressions.

## Start or reuse the browser build

1. If the generated page is missing or stale, run the workspace task
   **Temporary Build WebAssembly Release**. Emscripten must be installed and
   activated (`EMSDK` set); see `docs/building-web.md`.
2. The self-contained output is
   `gladius/out/build/web-release/web/gladius.html`. Open that absolute path as
   a `file:///.../gladius.html` URL with `open_browser_page`. The page can run
   directly from `file://`; a local HTTP server is not required for this
   packaged output. Reuse an existing browser page when available.
3. Keep the returned page ID and pass it to subsequent browser actions. After a
   rebuild, use `navigate_page` with `type: "reload"` on the existing page.
4. Wait for startup to finish before evaluating the UI. A visible
   `#gladius-status` message such as “WebGPU is not available in this browser”
   or “Gladius startup failed” is a startup blocker. Check both the message and
   the rendered page; some Gladius WebGPU diagnostic messages are logged at
   console error severity even when rendering is working.

The browser shell is `gladius/web/shell.html`. Changes there are consumed at
Emscripten link time and may not trigger an incremental relink; after shell
changes, verify that the generated HTML was actually regenerated.

## Browser controls and screenshots

Load the browser tools with tool search if they are not available in the
session. Prefer the dedicated controls before writing Playwright code:

| Goal | Browser control |
| --- | --- |
| Open a page or reuse a page | `open_browser_page` |
| Inspect accessibility/DOM and recent browser events | `read_page` |
| Capture the visible UI | `screenshot_page` |
| Operate real HTML elements, if any | `click_element`, `type_in_page`, `hover_element` |
| Navigate or reload after a build | `navigate_page` |
| Canvas coordinates, drag gestures, or other unsupported input | `run_playwright_code` |

For this app, capture the whole viewport with `screenshot_page` and inspect the
returned image directly. It is visual evidence in the conversation, not
necessarily a screenshot file saved into the repository. Use a selector or
reference only when you intentionally need to crop a real DOM element.

For canvas interaction:

1. Take a screenshot first and locate the target in the current viewport.
2. Use `run_playwright_code` with the page ID and a short, focused Playwright
   action. For example, `await page.mouse.click(x, y)` clicks the screenshot-derived
   viewport coordinate. Recalculate coordinates after resizing or changing
   panels; do not rely on coordinates from an old screenshot.
3. Use `page.mouse.move`, `page.mouse.down`, and `page.mouse.up` for drags;
   specify the right mouse button for camera gestures when needed. Use
   `page.keyboard.press(...)` for keyboard shortcuts after focusing the canvas.
4. Capture another screenshot after each meaningful state change. Use
   `click_element` only for actual DOM elements—clicking `#gladius-canvas` by
   locator usually clicks its center, not the intended ImGui control.

Keep Playwright snippets concise and self-contained. Do not access `document`
or `window` directly from `run_playwright_code`; use the supplied `page` API.
Use `run_playwright_code` only when the dedicated browser actions cannot perform
the required interaction.

## Gather useful UI feedback

Before proposing a redesign, establish the user’s task and the problem being
solved. If unclear, ask focused questions such as:

- Who is doing the task, and what are they trying to accomplish?
- At which step do they hesitate, get blocked, or lose context?
- What outcome would make the flow feel successful, and what should not change?

Avoid a vague “What do you think?” prompt. Separate observed friction from
possible design solutions, and record the user’s exact task, expected behavior,
actual behavior, and priority.

For a reproducible visual review:

1. Pick a representative document/model and a short task flow. Avoid modifying
   the user’s unsaved work without agreement.
2. Record the viewport, open panels, active tab, document/model, and relevant
   camera or zoom state. Capture a baseline screenshot before interacting.
3. Exercise the flow one step at a time. Capture screenshots for important
   states (for example, an open menu, a selected panel, an error, or a completed
   task), not just the final screen.
4. Compare before and after with the same document, viewport, and UI state.
   Check hierarchy, discoverability, readability, feedback, and recovery from
   errors against the user’s stated task—not personal aesthetic preference.
5. Report actionable findings with severity, exact reproduction steps,
   expected versus actual behavior, and the relevant screenshot. Label visual
   observations as observations and suggestions as suggestions.

## Validate UI changes

- Rebuild with **Temporary Build WebAssembly Release**, reload the generated
  page, and repeat the same baseline and task flow. If the change is in
  `shell.html`, confirm a relink occurred.
- Check startup/status messages, the visible result, and relevant browser
  events. Do not treat every `[WebGPU]` console entry logged as an error as a
  failure by itself; corroborate it with an explicit startup failure, an
  uncaught exception, or a broken/missing rendered state.
- Verify functional behavior as well as appearance. A screenshot cannot prove
  model operations, file handling, or rendering correctness; run the relevant
  project tests for those behaviors and report any browser-only limitations.
- For file-import flows, use a known `.3mf` fixture and verify the in-app result.
  The browser shell supports dropping `.3mf` files onto the canvas and opening
  an HTTP(S) model URL via its `url` query parameter. Cross-origin URLs require
  CORS; browser virtual files are lost on reload.
- Finish with the changed workflow, test/build result, screenshots reviewed,
  and any remaining WebGPU, viewport, or browser limitations.
