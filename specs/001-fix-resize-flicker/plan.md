# Implementation Plan: Fix Resize Flicker

**Branch**: `001-fix-resize-flicker` | **Date**: 2026-01-06 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-fix-resize-flicker/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/commands/plan.md` for the execution workflow.

## Summary

Eliminate render area clearing during window resize operations by preserving the existing frame buffer content. Currently, window resize triggers `invalidateView()` which clears the render state and causes visible flicker. The solution will modify the resize handling to preserve displayed content while adjusting viewport dimensions, ensuring smooth visual continuity during all window operations (manual resize, maximize/restore, multi-monitor transitions).

## Technical Context

**Language/Version**: C++20 (modern C++ with STL, smart pointers, move semantics)  
**Primary Dependencies**: OpenCL 1.2+ (GPU compute), OpenGL (rendering), ImGui (UI framework), CMake/Ninja (build)  
**Storage**: N/A (real-time rendering application, no persistent data storage for this feature)  
**Testing**: GoogleTest/GMock (unit tests), manual visual testing for resize operations  
**Target Platform**: Linux (primary), cross-platform desktop (Windows/Mac secondary)  
**Project Type**: Single desktop application with GPU-accelerated 3D rendering  
**Performance Goals**: Maintain 60fps during resize operations (<16ms per frame), no visible discontinuity  
**Constraints**: Must preserve OpenGL/OpenCL interop buffers, UI thread operations only for GL, async rendering must not block  
**Scale/Scope**: Single-window 3D viewport, ~150 lines of affected code in RenderWindow class

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Compliance Status: ✅ PASSING

#### Modern C++ Standards ✅
- **Status**: Compliant
- **Evidence**: Existing codebase uses C++20, smart pointers, STL algorithms, constexpr, move semantics
- **Plan**: Continue using modern C++ patterns for resize handling logic

#### Test-First Development ✅  
- **Status**: Compliant with caveat
- **Evidence**: Project has comprehensive GTest suite in `gladius/tests/unittests/`
- **Plan**: Add manual visual test cases for resize behavior (automated UI testing not required per constitution)
- **Rationale**: Window resize flickering is a visual UX issue best verified through manual testing; no automated UI test framework exists in project

#### Simplicity First (KISS, DRY, YAGNI) ✅
- **Status**: Compliant
- **Evidence**: Solution avoids complex double-buffering, reuses existing frame buffer mechanism
- **Plan**: Minimal changes to `RenderWindow::renderWindow()` resize detection logic (~10-20 lines modified)
- **YAGNI**: No premature optimization for multi-window or advanced scenarios

#### Consistent Code Style ✅
- **Status**: Compliant
- **Evidence**: Will follow Allman braces, 4-space indent, camelCase, PascalCase per constitution
- **Plan**: Match existing RenderWindow.cpp style exactly

#### Documentation ✅
- **Status**: Compliant
- **Evidence**: Will add inline comments explaining resize preservation logic
- **Plan**: Doxygen comments not required (private method), inline comments sufficient

### Constitution Violations: None

No complexity violations requiring justification.

## Project Structure

### Documentation (this feature)

```text
specs/001-fix-resize-flicker/
├── plan.md              # This file (implementation plan)
├── research.md          # Phase 0: Resize handling patterns, GL buffer preservation
├── data-model.md        # N/A (no data entities for this feature)
├── quickstart.md        # Phase 1: Developer testing guide for resize behavior
├── contracts/           # N/A (no API contracts for this feature)
└── checklists/
    └── requirements.md  # Specification validation checklist (completed)
```

### Source Code (repository root)

```text
gladius/
├── src/
│   └── ui/
│       ├── RenderWindow.h      # Declarations for RenderWindow class
│       └── RenderWindow.cpp    # Implementation (resize handling at lines 488-503)
└── tests/
    └── unittests/
        └── ui/                 # Manual visual test instructions
            └── RenderWindowResizeTest.md  # Test protocol for resize behavior
```

**Structure Decision**: Single project structure with modifications localized to the `RenderWindow` class in `gladius/src/ui/`. No new files required; changes are confined to existing resize detection logic (~lines 488-503) and the `invalidateView()` method (~lines 612-623). Testing will use manual visual verification following documented test protocol, consistent with the project's approach to UI/UX validation.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

No complexity violations detected. The solution maintains simplicity by:
- Reusing existing frame buffer and texture mechanisms
- No introduction of new architectural patterns or abstractions
- Minimal code changes (~10-20 lines modified in existing methods)
- No new dependencies or external libraries
