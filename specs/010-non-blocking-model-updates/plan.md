# Implementation Plan: Non-Blocking Model Updates

**Branch**: `010-non-blocking-model-updates` | **Date**: 2026-01-09 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/010-non-blocking-model-updates/spec.md`

## Summary

Ensure UI remains responsive when modifying models in the ModelEditor - both for graph changes (triggering code generation/recompilation) and parameter changes (triggering preview/bounding box updates). The preview window should show a busy indicator overlay while compute operations complete.

**Technical Approach**: Leverage existing async infrastructure from spec 003-async-preview-rendering, extend busy indicator coverage to all compute states, and eliminate remaining blocking patterns on the UI thread.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: OpenCL 1.2+, OpenGL, ImGui  
**Storage**: N/A  
**Testing**: GTest/GMock, GPU tests gated by `GLADIUS_RUN_GPU_TESTS=1`  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Single monolithic application  
**Performance Goals**: 60fps UI responsiveness during all editing operations  
**Constraints**: No main thread blocking during compute operations  
**Scale/Scope**: Desktop CAD application for 3D printing

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ | ✅ | Uses async/await patterns, smart pointers |
| II. Test-First | ⚠️ | Tests needed for busy indicator and blocking removal |
| III. Simplicity (KISS/DRY/YAGNI) | ✅ | Extends existing async infrastructure |
| IV. Code Style | ✅ | Follows established patterns |
| V. Documentation | ⚠️ | Doxygen comments needed for new/modified APIs |

## Project Structure

### Documentation (this feature)

```text
specs/010-non-blocking-model-updates/
├── plan.md              # This file
├── research.md          # Phase 0 output (existing patterns analysis)
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (to be created)
```

### Source Code (affected files)

```text
gladius/src/
├── compute/
│   ├── ComputeCore.cpp         # Parameter update, bounding box, SDF computation
│   └── ComputeCore.h           # Async API declarations
├── ui/
│   ├── MainWindow.cpp          # Model update loop, parameter dirty handling
│   ├── ModelEditor.cpp         # Graph editing, parameter modification
│   ├── NodeView.cpp            # Parameter change detection
│   └── RenderWindow.cpp        # Busy indicator, blocking patterns
└── Document.cpp                # refreshModelAsync, updateParameter
```

---

## Phase 0: Research - Existing Infrastructure Analysis

### What Already Exists

| Component | Location | Status | Notes |
|-----------|----------|--------|-------|
| `refreshModelAsync()` | Document.cpp:99-127 | ✅ Exists | Background thread for graph changes |
| `precomputeSdfAsync()` | ComputeCore.cpp:1088+ | ✅ Exists | Async SDF computation with cl::Event |
| `requestComputeToken()` | ComputeCore.cpp:74-81 | ✅ Exists | Non-blocking mutex acquisition |
| `tryToupdateParameter()` | ComputeCore.cpp:174-186 | ✅ Exists | Non-blocking parameter update |
| Busy indicator | RenderWindow.cpp:580-608 | ✅ Exists | Shows during `!isRendererReady() \|\| isAnyCompilationInProgress()` |
| Async preview rendering | RenderWindow.cpp | ✅ Exists | Spec 003 phases 1-3 complete |
| Triple buffering | AsyncRenderController | ✅ Exists | Front/back/pending buffer system |

### Remaining Blocking Patterns

| Blocking Call | Location | Impact | Resolution |
|--------------|----------|--------|------------|
| `waitForComputeToken()` | RenderWindow.cpp:268 | Blocks main thread | Replace with `requestComputeToken()` or remove |
| `waitForComputeToken()` | RenderWindow.cpp:2387 | Blocks in coroutine | Move to worker thread or restructure |
| `updateBBox()` | Calls `queue.finish()` under mutex | GPU sync blocks UI | Already called from background in coroutine |
| `updateParameter()` in MainWindow | Called on dirty flag | Potentially blocking | Already uses `tryToupdateParameter()` ✅ |

### Busy Indicator Coverage Gap

Current trigger conditions:
```cpp
if (!m_core->isRendererReady() || m_core->isAnyCompilationInProgress())
```

**Missing triggers**:
- Bounding box computation in progress
- SDF precomputation in progress (when not part of compilation)
- Parameter buffer update in progress (negligible, probably not needed)

---

## Phase 1: Design

### Approach 1: Extend Busy Indicator Coverage (Recommended)

Add state tracking for SDF/bounding box computation:

```cpp
// ComputeCore.h - new state queries
[[nodiscard]] bool isBoundingBoxComputationInProgress() const;
[[nodiscard]] bool isSdfComputationInProgress() const;

// RenderWindow.cpp - extend busy indicator trigger
bool const showBusyIndicator = !m_core->isRendererReady() 
    || m_core->isAnyCompilationInProgress()
    || m_core->isSdfComputationInProgress()
    || m_core->isBoundingBoxComputationInProgress();
```

### Approach 2: Remove Remaining Blocking Patterns

The `waitForComputeToken()` at RenderWindow.cpp:268 is the critical one. Options:

**Option A**: Use `requestComputeToken()` and skip frame if busy
```cpp
auto token = m_core->requestComputeToken();
if (!token.has_value())
{
    // Show last frame with busy indicator
    return;
}
```

**Option B**: Since spec 003 async preview is implemented, check if this code path is even reached during normal operation. The async path may already handle this.

### Decision

1. **Extend busy indicator** to cover SDF and bounding box states
2. **Investigate** whether RenderWindow.cpp:268 is actually blocking the async path
3. **Keep** existing async infrastructure (refreshModelAsync, tryToupdateParameter)

---

## Complexity Tracking

> No constitution violations expected - this extends existing patterns

| Item | Justification |
|------|---------------|
| Minimal new code | Primarily adding state queries and extending existing condition |

---

## Implementation Tasks (High-Level)

### Phase 1: Add Compute State Tracking

1. Add `m_sdfComputationInProgress` atomic flag to ComputeCore
2. Add `m_boundingBoxComputationInProgress` atomic flag to ComputeCore
3. Set/clear flags in `precomputeSdfAsync()` and `updateBoundingBoxFast()`
4. Add public query methods

### Phase 2: Extend Busy Indicator

5. Update busy indicator condition in RenderWindow.cpp:581
6. Verify busy indicator shows during graph edit → recompile cycle
7. Verify busy indicator shows during parameter change → SDF update cycle

### Phase 3: Remove Blocking Patterns (if needed)

8. Analyze if RenderWindow.cpp:268 `waitForComputeToken()` is reached in async mode
9. If blocking, replace with non-blocking pattern
10. Same analysis for RenderWindow.cpp:2387

### Phase 4: Testing & Validation

11. Manual test: Parameter slider responsiveness
12. Manual test: Graph edit responsiveness  
13. Manual test: Busy indicator visibility
14. Add unit tests for state tracking

---

## Next Steps

Run `/speckit.tasks` to generate detailed task breakdown with dependencies and acceptance criteria.
