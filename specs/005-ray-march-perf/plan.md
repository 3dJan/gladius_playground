# Implementation Plan: Ray Marching Performance Optimization

**Branch**: `005-ray-march-perf` | **Date**: 2026-01-03 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/005-ray-march-perf/spec.md`

## Summary

Optimize the ray marching (sphere tracing) renderer to achieve 30% faster HQ rendering and 20% fewer ray steps. Key techniques: adaptive over-relaxation per-ray based on SDF gradient, reusing low-res preview distances for HQ initialization, and debug instrumentation for performance metrics.

## Technical Context

**Language/Version**: C++20, OpenCL 1.2  
**Primary Dependencies**: OpenCL (GPU compute), OpenGL (rendering), ImGui (UI/debug overlay)  
**Storage**: N/A (in-memory GPU buffers)  
**Testing**: GTest/GMock; baseline via thumbnail rendering on existing 3mf test files  
**Target Platform**: Linux (primary), Windows; GPU with OpenCL 1.2+ support  
**Project Type**: Single project (monorepo)  
**Performance Goals**: 30% faster HQ render, 20% fewer ray steps, 30+ FPS camera interaction  
**Constraints**: OpenCL 1.2+ compatibility, no dynamic GPU memory allocation, max 32-level BVH stack  
**Scale/Scope**: Single-user desktop application, viewport renders up to ~4K resolution

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | OpenCL kernels follow C99; host code uses C++20 |
| II. Test-First Development | ✅ Pass | Integration tests with 3mf files; baseline metrics stored |
| III. Simplicity First (KISS/DRY/YAGNI) | ✅ Pass | Reusing existing low-res preview instead of new pre-pass |
| IV. Consistent Code Style | ✅ Pass | Following existing kernel/host code patterns |
| V. Documentation and Comments | ✅ Pass | Doxygen for public APIs; kernel comments as per existing style |

**Gate Result**: ✅ PASS — No constitution violations; proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/005-ray-march-perf/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A for this feature - no API contracts)
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/
├── src/
│   ├── kernel/
│   │   ├── rendering.cl       # Core rayCast() function - PRIMARY EDIT TARGET
│   │   ├── renderer.cl        # renderScene kernel entry point
│   │   └── rendering.h        # Kernel header with RenderingSettings flags
│   ├── compute/
│   │   ├── Rendering.h/cpp    # Host-side rendering orchestration
│   │   └── ComputeCore.h/cpp  # Compute context management
│   ├── ui/
│   │   ├── RenderWindow.h/cpp # Async job scheduling, low-res preview
│   │   └── render/
│   │       ├── AsyncRenderController.h  # Job queue, triple buffering
│   │       └── AsyncRenderTypes.h       # RenderJob, FrameState types
│   └── RenderProgram.h/cpp    # OpenCL kernel wrapper
└── tests/
    ├── integrationtests/       # 3mf-based rendering tests
    └── unittests/              # Unit tests (new debug metrics tests)
```

**Structure Decision**: Single project structure. All changes are within `gladius/src/kernel/` (OpenCL) and `gladius/src/ui/` (async rendering integration). No new projects or major restructuring needed.

## Complexity Tracking

> No constitution violations requiring justification.
