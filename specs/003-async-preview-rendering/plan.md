# Implementation Plan: Asynchronous Preview Rendering

**Branch**: `003-async-preview-rendering` | **Date**: 2026-01-02 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/003-async-preview-rendering/spec.md`

## Summary

Move low-resolution preview rendering from the UI thread to the existing async coroutine backend to maintain 55+ FPS during camera movement. The infrastructure (`AsyncRenderController`, `RenderJobType::LowResPreview`, triple buffering) already exists—this plan focuses on implementing the async preview job executor and eliminating blocking calls from the UI thread.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: OpenCL 1.2+, OpenGL, ImGui, libcoro (coroutines)  
**Storage**: N/A (GPU buffers only)  
**Testing**: GTest/GMock, `GLADIUS_RUN_GPU_TESTS=1` for GPU tests  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Single desktop application  
**Performance Goals**: 55+ FPS during camera movement, <100ms preview latency  
**Constraints**: No blocking GPU calls (`glFinish`, `clFinish`) on UI thread  
**Scale/Scope**: Single-user desktop application

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Using C++20 coroutines, smart pointers, atomics |
| II. Test-First Development | ✅ Pass | Unit tests for async preview job, integration test for FPS |
| III. Simplicity First | ✅ Pass | Reusing existing AsyncRenderController infrastructure |
| IV. Consistent Code Style | ✅ Pass | Following existing patterns in RenderWindow.cpp |
| V. Documentation | ✅ Pass | Doxygen for new public APIs |

**Gate Result**: PASS - No violations, proceed to Phase 0

### Post-Design Re-Check (after Phase 1)

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Design uses `std::atomic`, `std::optional`, `cl::Event` |
| II. Test-First Development | ✅ Pass | Tests defined: epoch cancellation, buffer lifecycle, FPS validation |
| III. Simplicity First | ✅ Pass | Reusing existing triple buffer; no new dependencies |
| IV. Consistent Code Style | ✅ Pass | Follows `executeAsyncRenderJob()` pattern exactly |
| V. Documentation | ✅ Pass | contracts/async-preview-api.md defines all interfaces |

**Post-Design Gate Result**: PASS - Design aligns with constitution

## Project Structure

### Documentation (this feature)

```text
specs/003-async-preview-rendering/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (state machine diagrams)
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (interface contracts)
└── tasks.md             # Phase 2 output
```

### Source Code (affected files)

```text
gladius/src/
├── ui/
│   ├── RenderWindow.cpp           # MODIFY: Add async preview path
│   ├── RenderWindow.h             # MODIFY: Add async preview state
│   └── render/
│       ├── AsyncRenderController.h    # MODIFY: Add preview-specific helpers
│       ├── AsyncRenderController.cpp  # MODIFY: Preview buffer management
│       └── AsyncRenderTypes.h         # EXISTS: RenderJobType::LowResPreview
├── compute/
│   ├── ComputeCore.cpp            # MODIFY: Add non-blocking preview render
│   └── ComputeCore.h              # MODIFY: Add async preview API
├── GLImageBuffer.cpp              # REVIEW: Ensure non-blocking bind path
└── GLImageBuffer.h                # REVIEW: Add invalidate-without-transfer

gladius/tests/unittests/
└── AsyncPreviewRendering_tests.cpp  # NEW: Unit tests for async preview
```

**Structure Decision**: Minimal changes to existing files; reuse AsyncRenderController infrastructure

## Complexity Tracking

> No constitution violations; this section left intentionally minimal.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | N/A | N/A |
