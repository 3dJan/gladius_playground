# Implementation Plan: Event Viewer UX Improvements

**Branch**: `007-event-viewer-ux` | **Date**: 2026-01-04 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/007-event-viewer-ux/spec.md`

## Summary

Improve the Event Viewer (LogView) UX by:
1. Defaulting severity filter to hide Info-level messages (show only Warning, Error, Fatal)
2. Adding clipboard copy functionality for individual events and bulk operations
3. Adding copy buttons in collapsed view tooltips for per-severity copying

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (UI rendering), fmt (string formatting)  
**Storage**: N/A (in-memory event log, session-only filter preferences)  
**Testing**: GTest/GMock (unit tests in `gladius/tests/unittests/`)  
**Target Platform**: Linux, Windows, macOS (cross-platform via CMake)  
**Project Type**: Single desktop application  
**Performance Goals**: Copy operations must complete instantly (<100ms for any reasonable log size)  
**Constraints**: ImGui clipboard API (`ImGui::SetClipboardText`) supports text only; no rich text  
**Scale/Scope**: Typically <10,000 events in a session; copy operations on filtered subsets

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Will use C++20 features, STL algorithms, const correctness |
| II. Test-First Development | ✅ PASS | Unit tests for format functions; manual testing for UI interactions |
| III. Simplicity First (KISS, DRY, YAGNI) | ✅ PASS | Minimal changes to existing LogView; reuse existing filter infrastructure |
| IV. Consistent Code Style | ✅ PASS | Follow existing LogView.cpp conventions |
| V. Documentation and Comments | ✅ PASS | Document new public methods with Doxygen |

**Gate Result**: ✅ PASS - All principles satisfied, no violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/007-event-viewer-ux/
├── plan.md              # This file
├── research.md          # Phase 0 output (ImGui patterns, best practices)
├── data-model.md        # Phase 1 output (Event format specification)
├── quickstart.md        # Phase 1 output (Developer guide)
├── contracts/           # N/A - no external APIs
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (affected files)

```text
gladius/src/ui/
├── LogView.h            # Add clipboard methods, update default filter state
└── LogView.cpp          # Implement copy functionality, context menus

gladius/tests/unittests/ui/
└── LogView_tests.cpp    # NEW: Unit tests for event formatting
```

**Structure Decision**: This is a modification to an existing UI component (LogView). No new files needed except for unit tests. The clipboard formatting logic will be added as private helper methods in LogView.

## Complexity Tracking

No violations detected. Feature is straightforward UI enhancement with no architectural changes.

## Post-Design Constitution Re-Check

*Re-evaluated after Phase 1 design completion.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Using fmt library, std::chrono, STL algorithms |
| II. Test-First Development | ✅ PASS | LogView_tests.cpp planned for format functions |
| III. Simplicity First | ✅ PASS | ~150 lines of changes to existing file; no new abstractions |
| IV. Consistent Code Style | ✅ PASS | Following existing LogView.cpp patterns (Allman braces, naming) |
| V. Documentation | ✅ PASS | New helper functions will have Doxygen comments |

**Final Gate Result**: ✅ PASS - Ready for task generation.

## Generated Artifacts

| Artifact | Path | Status |
|----------|------|--------|
| Research | [research.md](research.md) | ✅ Complete |
| Data Model | [data-model.md](data-model.md) | ✅ Complete |
| Quickstart | [quickstart.md](quickstart.md) | ✅ Complete |
| Contracts | N/A | Not applicable (no external APIs) |
| Agent Context | `.github/agents/copilot-instructions.md` | ✅ Updated |

## Next Steps

Run `/speckit.tasks` to generate the task breakdown for implementation.
