# Agent / Automation Onboarding (Gladius)

This guide is for **coding agents** (and humans driving them) working in this repository.

It focuses on how to navigate the codebase safely, how to validate changes, and how to avoid common pitfalls (GPU/OpenCL, async/thread-safety, build system conventions).

## Golden rules in this repo

1. **Use the VS Code tasks** to build and test.
   - Do **not** invoke `cmake`/`ninja` manually unless the repo explicitly asks for it.
2. **Assume OpenCL/GPU may be missing** on CI or on a contributor’s machine.
   - Prefer workflows that work without GPU; gate GPU tests with env vars and skip messages.
3. **Be paranoid about thread-safety**.
   - The app uses async refresh/export pipelines. Shared mutable state must be synchronized.
4. **Keep diffs small and targeted**.
   - Avoid drive-by formatting changes.

## “Where do I start?” (entry points)

### Application lifecycle

- `gladius/src/main.cpp`
  - CLI parsing, headless mode, MCP enablement
- `gladius/src/Application.{h,cpp}`
  - owns `ui::MainWindow`
  - can run headless (`setupHeadless`) and/or start MCP server

### User/session state

- `gladius/src/Document.{h,cpp}`
  - load/save/merge
  - async refresh model
  - export pipelines (`exportAsStl`, etc.)
  - parameter accessors (`setFloatParameter`, …)

### Compute + OpenCL compilation

- `gladius/src/compute/ComputeCore.*`
- `gladius/src/compute/ProgramManager.*`

### IO

- `gladius/src/io/…`
- 3MF-specific logic: `gladius/src/io/3mf/…`

### Meshing / MDC

- STL exporter entry: `gladius/src/io/ManifoldDualContouringStlExporter.*`
- GPU MDC pipeline: `gladius/src/compute/ManifoldDualContouringGpu.*`

## Build & test validation (agent-friendly)

### Fast validation loop

- Build: **Build ALL (linux-releaseWithDebug)**
- Tests: **Run Gladius Tests (linux-releaseWithDebug)**

### If OpenCL isn’t available

Use the “no OpenCL tests” preset/workflow (or ask the human to do so):

- configure preset: `linux-releaseWithDebug-noOpenCL`
- test preset: `ReleaseWithDebug-noOpenCL`

This disables OpenCL-dependent tests via `ENABLE_OPENCL_TESTS=OFF`.

### GPU regression tests

Only run GPU regressions when explicitly requested or when working on GPU/MDC code.
Existing tasks use:

- `GLADIUS_RUN_GPU_TESTS=1`

…and sometimes additional debug toggles:

- `GLADIUS_DEBUG_MDC_CONFIG=1`
- `GLADIUS_MDC_DISABLE_DEPTH_CLAMP=1`

## Common pitfalls and how to avoid them

### 1) Async exporter data races

If you add/modify async exporters that run work on background threads, avoid writing progress/error strings directly into members that the UI reads without synchronization.

Safer patterns:

- return results through the `std::future` / async result object
- marshal state updates back to the UI thread
- protect shared members with a mutex (or atomics for simple flags)

(See `REVIEW_ACTIONS.md` for a list of existing race-risk hotspots.)

### 2) Don’t accidentally make GPU required

- If a test requires GPU: gate with `GLADIUS_RUN_GPU_TESTS` and `GTEST_SKIP()` with a descriptive reason.
- If a code path uses GPU opportunistically: ensure graceful fallback to CPU/skip path.

### 3) Locking and compilation state

`ProgramManager` and `ComputeCore` coordinate compilation and compute tokens.

When changing compilation state queries or “NoLock” methods:

- make it explicit which mutex must be held
- avoid mixing lock orders between different mutexes

### 4) Packaging/runtime library paths

On Linux, the project bundles certain shared libraries (e.g., lib3mf) and uses `$ORIGIN` / launch scripts.

If you change install or runtime paths:

- verify both **build-tree execution** (tests) and **install-tree execution** (packaging)

## Guidance for writing docs or plans in this repo

This repo already contains a lot of design notes under `thegreatplan/`. Those are useful for context but can be out-of-date.

For stable docs:

- put developer-facing onboarding under `docs/`
- keep it factual and tied to code/presets/tasks
- link to the exact file paths that are the canonical references

## Suggested “agent brief” template

When starting work on an issue, fill this out (in your own notes) before editing code:

- Goal: (one sentence)
- User-visible behavior change: (yes/no)
- Affected subsystems: (UI / compute / IO / tests)
- Validation plan: (which tasks/tests)
- GPU dependency: (none / optional / required)
- Risk areas: (thread-safety, file IO, OpenCL, numerical)

