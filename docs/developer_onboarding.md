# Developer Onboarding (Gladius)

This document is a practical onboarding guide for developers working on **Gladius**.

Gladius is a C++20 application and library for processing **3MF Volumetric / Implicit** geometry. It uses **OpenCL** for compute-heavy tasks and **OpenGL + ImGui** for interactive UI.

## Quick orientation

### What you’re building

- **GUI application**: interactive node/graph editor + viewport
- **Library-style core**: compute + IO pipelines used by the app and tests
- **Optional MCP server**: HTTP/stdio JSON-RPC server for agent control

### Where things live

- `gladius/src/` — main application + shared core code
  - `Application.*` — application lifecycle (UI, headless, MCP enable/disable)
  - `Document.*` — the “current project/session”: load/save/import/export, parameters, assembly, mesh generation
  - `ui/` — ImGui UI + viewport plumbing
  - `compute/` — OpenCL program compilation, kernels, compute pipelines
  - `io/` — import/export (3MF, STL, VDB, contours, …)
  - `nodes/` — graph/node system and model definitions
  - `kernel/` — OpenCL kernels/resources bundled into the binary
- `gladius/tests/` — GoogleTest-based tests
- `gladius/vcpkg.json` — dependency manifest
- `gladius/CMakePresets.json` — canonical configure + test presets

## Prerequisites

For a full walkthrough of prerequisites and first-time setup (OS packages, compiler, OpenCL runtime, vcpkg, IDE configuration), see the platform build guides:

- **[Building on Linux](building-linux.md)**
- **[Building on Windows](building-windows.md)**

## Build (recommended: VS Code tasks)

This repo is set up to be built via **VS Code tasks**.

### Recommended task

- **Build ALL (linux-releaseWithDebug)**

This corresponds to the CMake preset:

- configure preset: `linux-releaseWithDebug` (`RelWithDebInfo`)
- build output: `gladius/out/build/linux-releaseWithDebug/`

### If you don’t have OpenCL available

There is a “no OpenCL tests” preset:

- configure preset: `linux-releaseWithDebug-noOpenCL`
- test preset: `ReleaseWithDebug-noOpenCL`

That preset sets `ENABLE_OPENCL_TESTS=OFF` so OpenCL-dependent tests won’t be compiled/enabled.

## Running

### From the build tree

After building, executables are produced under the build directory, typically:

- `gladius/out/build/linux-releaseWithDebug/src/…`

(Exact executable locations vary by target. If in doubt, search the build tree for `gladius`.)

### CLI options

The main binary supports:

- `--help` — show usage
- `--headless` — run without starting the UI
- `--debug-opencl` — enable OpenCL debug output (kernel validation/buffer checks)

If built with MCP support (`GLADIUS_ENABLE_MCP`):

- `--mcp-server [port]` — start MCP server using HTTP transport (default port 8080)
- `--mcp-stdio` — start MCP server using stdio transport (intended for editor/agent integration)

See `gladius/src/main.cpp` for the canonical argument parsing.

### MCP server docs

- `gladius/MCP_IMPLEMENTATION.md` — architectural overview and protocol examples
- `gladius/documentation/manuals/MCP_IMPLEMENTATION_SUMMARY.md` — shorter summary

## Tests

### Run the full test suite

Use the task:

- **Run Gladius Tests (linux-releaseWithDebug)**

This uses the CMake test preset `ReleaseWithDebug`.

### GPU / OpenCL-gated tests

Some tests are gated by environment variables. Common ones used by existing tasks:

- `GLADIUS_RUN_GPU_TESTS=1` — enable GPU-specific test execution
- `GLADIUS_DEBUG_MDC_CONFIG=1` — extra MDC config logging
- `GLADIUS_MDC_DISABLE_DEPTH_CLAMP=1` — toggle depth clamp behavior for GPU regressions

If you are adding a new GPU test, prefer:

- clear `GTEST_SKIP()` messages when prerequisites are missing
- gating via an env var (consistent with `GLADIUS_RUN_GPU_TESTS` pattern)

## Common development workflows

### “I’m changing compute code”

- Start at `gladius/src/compute/ComputeCore.*` and `gladius/src/compute/ProgramManager.*`.
- Many codepaths are lock-sensitive. Avoid introducing cross-thread shared state without mutex/atomics.
- If you touch OpenCL kernels, look under `gladius/src/kernel/`.

### “I’m changing meshing (MDC)”

- Document entry points often live in `Document::exportAsStl(...)`.
- The MDC STL exporter is in `gladius/src/io/ManifoldDualContouringStlExporter.*`.
- GPU-related MDC code is in `gladius/src/compute/ManifoldDualContouringGpu.*`.

### “I’m changing 3MF IO”

- Look under `gladius/src/io/3mf/`.
- `Document::load/saveAs/merge` are good top-level entry points.

### “I’m changing UI”

- UI entry is in `gladius/src/ui/`.
- The app lifecycle is managed via `gladius::Application` and `gladius::ui::MainWindow`.

## Adding dependencies

- Add the dependency to `gladius/vcpkg.json`.
- Prefer existing vcpkg ports.
- Keep dependency surface minimal; this project already pulls in substantial graphics/compute stack.

## Coding conventions (high-level)

The repo enforces conventions (see `.github/copilot-instructions.md`):

- Allman braces, 4-space indentation
- `.h` declarations, `.cpp` definitions
- `#pragma once` include guards
- camelCase for functions/variables; PascalCase for types
- prefer STL and smart pointers
- exceptions for error handling (avoid error codes)
- Doxygen-style comments for public APIs

## “If you’re stuck” checklist

- Build issues: check `VCPKG_ROOT`, OpenCL headers/runtime, and preset selection.
- Runtime issues: verify OpenCL device availability and driver correctness.
- Test flakiness: try the `noOpenCL` preset to isolate non-GPU failures.

## Further reading

- User-level mental model and concepts: `gladius/documentation/manuals/user-guide.md`
- Larger architectural plans/notes live under `thegreatplan/` (these are research/design notes, not always up-to-date).
