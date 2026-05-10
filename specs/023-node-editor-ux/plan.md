# Implementation Plan: Node Editor UX Improvements

**Branch**: `023-node-editor-ux` | **Date**: 2026-03-14 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/023-node-editor-ux/spec.md`

## Summary

Deliver a polished first-pass node editor UX by standardizing interaction geometry across node types, introducing enhanced inline numeric widgets, improving begin/end and function-call usability, and protecting UI responsiveness during parameter edits. The implementation explicitly prioritizes stable left/right pin rails, shared pin behavior, and compact rounded/capsule nodes over experimental perimeter-mounted circular layouts.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui, imgui-node-editor, OpenGL, OpenCL, lib3mf, fmt  
**Storage**: 3MF document data plus node/parameter metadata persisted with the document; no new external storage  
**Testing**: GTest/GMock unit tests, existing integration tests, targeted manual UI verification  
**Target Platform**: Linux and Windows desktop application  
**Project Type**: Single desktop application  
**Performance Goals**: Maintain at least 30 fps during continuous parameter editing; link compatibility feedback visible within 1 frame of drag start  
**Constraints**: Must build and test via VS Code tasks; main/UI thread must remain responsive; compact nodes must share pin interaction behavior with regular nodes; avoid growing pre-existing oversized UI files where extraction is feasible  
**Scale/Scope**: UI-focused change across `gladius/src/ui/` rendering and interaction paths with supporting tests in `gladius/tests/unittests/` and `gladius/tests/integrationtests/`

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Requirement | Status | Notes |
|-------------|--------|-------|
| Modern C++ Standards | ✅ PASS | C++20 codebase; extracted helpers will follow existing STL/smart-pointer/error-handling patterns. |
| Test-First Development | ✅ PASS | Plan adds/extends unit tests for numeric widgets, link-drag compatibility, and throttling; visual/layout behavior also gets manual verification steps. |
| Simplicity First | ✅ PASS | Shared pin interaction model and compact-node redesign reduce special cases; true circular/perimeter pin experiments deferred. |
| Consistent Code Style | ✅ PASS | Plan follows existing naming/formatting conventions and avoids introducing parallel UI paradigms. |
| Documentation and Comments | ✅ PASS | Public helper APIs introduced in UI modules will require Doxygen comments; plan artifacts document rationale and boundaries. |
| UI Responsiveness | ✅ PASS | Dedicated throttle/coalescing behavior is planned; no blocking operations are introduced on the main thread. |

**Post-Design Check**: PASS — Phase 1 artifacts preserve the same shared-interaction strategy, avoid framework forks, and keep responsiveness/testing requirements explicit.

## Project Structure

### Documentation (this feature)

```text
specs/023-node-editor-ux/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── node-editor-interactions.md
│   └── widget-api.md
└── tasks.md
```

### Source Code (repository root)

```text
gladius/
├── src/
│   ├── ui/
│   │   ├── NodeView.h
│   │   ├── NodeView.cpp
│   │   ├── ModelEditor.h
│   │   ├── ModelEditor.cpp
│   │   ├── NumericWidgets.h
│   │   ├── NumericWidgets.cpp
│   │   ├── ParameterThrottle.h
│   │   ├── ParameterThrottle.cpp
│   │   ├── LinkDragState.h
│   │   ├── LinkDragState.cpp
│   │   ├── Style.h
│   │   ├── Style.cpp
│   │   └── LinkColors.h
│   └── nodes/
│       ├── Model.h
│       └── Model.cpp
└── tests/
   ├── unittests/
   └── integrationtests/
```

**Structure Decision**: Keep the feature within the existing single desktop application. Concentrate orchestration in `NodeView`/`ModelEditor`, move reusable widget and throttling logic into focused UI helper modules, and add tests alongside current unit/integration suites.

## Phase 0: Research Summary

1. **Shared inline interaction geometry is the primary UX foundation** — compact nodes must use the same pin interaction model as regular nodes.
2. **Compact visual identity should come from styling, not special hit-testing** — rounded/capsule bodies with left/right pin rails are the first-pass design.
3. **Numeric widgets belong in a dedicated helper module** — dial + drag-float behavior should be extracted from `NodeView` to keep rendering code maintainable.
4. **Port compatibility should be computed from the current model state and reused during a drag session** — not recomputed ad hoc per visual branch.
5. **Responsiveness needs explicit throttling/coalescing boundaries** — parameter edits update UI state immediately while recompute requests are deferred/coalesced.

## Phase 1: Design Plan

### Workstreams

1. **Shared Port Interaction Layer**
  - Standardize port sizing, hover behavior, tooltip behavior, drag-start reliability, and compatibility highlighting.
  - Ensure compact and regular nodes both use the same interaction contract.

2. **Compact Node Presentation System**
  - Rework compact nodes into a stable rounded/capsule layout with aligned pin rails and centered label/glyph treatment.
  - Define fallback expansion rules when compact layout cannot fit content without shrinking hit targets.

3. **Numeric Parameter Editing**
  - Introduce dial + drag-float pairing and optional slider presentation as a cohesive inline numeric editing model.
  - Map content type, bounds, and modifier behavior consistently.

4. **Begin/End and Function Call Usability**
  - Clarify inline editing, reordering/removal interactions, and visual distinction of signature nodes.
  - Improve discoverability of function-call navigation and binding status.

5. **Responsiveness and Verification**
  - Define throttling/coalescing behavior for parameter edits.
  - Cover widget logic, compatibility logic, and layout regressions with tests/manual quickstart flows.

### Implementation Strategy

1. Stabilize the port interaction baseline before changing compact visuals.
2. Extract reusable numeric/throttle helpers so `NodeView` remains a coordinator rather than a dumping ground.
3. Apply compact-node styling changes only after shared pin behavior is stable.
4. Improve begin/end and function-call UX on top of the shared node presentation system.
5. Finish with responsiveness tuning and regression verification.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Touching pre-existing large file `gladius/src/ui/NodeView.cpp` | `NodeView` remains the composition point for node rendering and must coordinate the redesign | A full rewrite or parallel renderer would increase risk and duplicate logic; instead, new behavior is extracted into helper modules while `NodeView` keeps orchestration responsibilities |
