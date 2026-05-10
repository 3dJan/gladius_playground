# Implementation Plan: Graph Error States

**Branch**: `015-graph-error-states` | **Date**: January 24, 2026 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/015-graph-error-states/spec.md`

## Summary

Replace endless error repetition during graph validation with a state-based error handling system. Validation issues are collected in a persistent `IssueList` that is displayed both in the Event Viewer (global) and as a collapsible overlay in the graph editor (context-specific). Code generation and preview rendering are gated on graph validity, preventing endless retry loops. During interactive editing, validation is silent (no events); on API/file loading, events are emitted once.

## Technical Context

**Language/Version**: C++20 (Clang/MSVC)  
**Primary Dependencies**: ImGui (UI), OpenCL 1.2+ (compute), lib3mf (file I/O), fmt (formatting)  
**Storage**: N/A (in-memory state only)  
**Testing**: GTest/GMock, build via CMake presets  
**Target Platform**: Linux/Windows desktops with GPU  
**Project Type**: Single desktop application  
**Performance Goals**: Validation completes <100ms; UI remains responsive at 60 fps  
**Constraints**: Debounce validation ~100-200ms during rapid editing  
**Scale/Scope**: Graphs with ~10-500 nodes typical

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ | Using C++20; smart pointers, STL, constexpr |
| II. Test-First Development | ✅ | Unit tests for IssueList, Validator changes |
| III. Simplicity First (KISS/DRY/YAGNI) | ✅ | Minimal new classes; extend existing Validator |
| IV. Consistent Code Style | ✅ | Follow existing naming, Allman braces |
| V. Documentation | ✅ | Doxygen for new public APIs |

**Gate Status**: ✅ PASS

## Project Structure

### Documentation (this feature)

```text
specs/015-graph-error-states/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (internal API contracts)
└── tasks.md             # Phase 2 output
```

### Source Code (modifications to existing structure)

```text
gladius/src/
├── nodes/
│   ├── Validator.h          # MODIFY: Add IssueType enum, fix suggestions
│   ├── Validator.cpp        # MODIFY: Populate IssueList instead of direct events
│   ├── IssueList.h          # NEW: Issue collection with query methods
│   └── IssueList.cpp        # NEW: Implementation
├── Document.h               # MODIFY: Add ValidationContext, separate validate from log
├── Document.cpp             # MODIFY: Gate updates on validity, context-aware logging
├── ui/
│   ├── ModelEditor.h        # MODIFY: Add collapsible issues overlay
│   ├── ModelEditor.cpp      # MODIFY: Render overlay, click-to-navigate
│   └── EventViewer.cpp      # MODIFY: Filter/display validation issues (if not existing)
└── EventLogger.h            # No changes needed (reuse existing Event type)

gladius/tests/unittests/
├── IssueList_Test.cpp       # NEW: Unit tests for IssueList
└── Validator_Test.cpp       # MODIFY: Tests for new validation behavior
```

**Structure Decision**: Extend existing code structure. New files limited to `IssueList.h/cpp` and corresponding test file. All other changes are modifications to existing files.

## Complexity Tracking

No constitution violations. All changes follow existing patterns.

---

## Phase 0 Complete: Research

See [research.md](research.md) for all resolved unknowns.

## Phase 1 Complete: Design & Contracts

### Artifacts Generated

| Artifact | Path | Description |
|----------|------|-------------|
| Data Model | [data-model.md](data-model.md) | Entity definitions: IssueList, ValidationIssue, IssueType, IssueSeverity, ValidationContext |
| API Contract | [contracts/issue-list-api.md](contracts/issue-list-api.md) | IssueList class interface specification |
| Quickstart | [quickstart.md](quickstart.md) | User and API usage guide |

### Post-Design Constitution Re-check

| Principle | Status | Verification |
|-----------|--------|--------------|
| I. Modern C++ Standards | ✅ | Uses C++20, smart pointers, STL containers, `[[nodiscard]]`, enum class |
| II. Test-First Development | ✅ | IssueList_Test.cpp planned; existing Validator_Test.cpp to be extended |
| III. Simplicity First | ✅ | Only 2 new files (IssueList.h/cpp); extends existing patterns |
| IV. Consistent Code Style | ✅ | Allman braces, camelCase methods, PascalCase types |
| V. Documentation | ✅ | Doxygen comments specified for all public APIs |

**Post-Design Gate Status**: ✅ PASS

---

## Planning Complete

**Next Step**: Run `/speckit.tasks` to generate implementation tasks.

### Summary

This plan implements state-based graph error handling by:

1. **New `IssueList` class** - Thread-safe container for validation issues
2. **Extended `ValidationIssue`** - Adds issue type, severity, fix suggestions, navigation IDs
3. **Modified `Validator`** - Populates IssueList instead of logging events directly
4. **Modified `Document`** - Owns IssueList, adds ValidationContext for event control
5. **Modified `ModelEditor`** - Adds collapsible issues overlay with click-to-navigate
6. **Update gating** - Code generation and rendering blocked when errors exist

Key user benefits:
- No more endless error loops
- Consolidated view of all issues
- Actionable fix suggestions
- Click-to-navigate to problem nodes
