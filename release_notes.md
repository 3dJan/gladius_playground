# Gladius Release Notes

## Version 1.2.14 (Unreleased)

### Features

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
