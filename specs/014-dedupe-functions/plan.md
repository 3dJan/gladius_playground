# Implementation Plan: Deduplicate Functionally Identical Functions

**Branch**: `014-dedupe-functions` | **Date**: 2026-01-24 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/014-dedupe-functions/spec.md`

## Summary

Implement a system to detect and remove mathematically identical implicit functions from 3MF assemblies. The solution involves creating a canonical comparison algorithm for `Model` graphs that ignores decorational data (names, IDs) while comparing structural topology, node types, and constant values. A UI button in the model editor outline triggers deduplication, which updates all `FunctionCall` references and external resource references to point to retained functions.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: GTest/GMock (testing), ImGui (UI), Lib3MF (file format)  
**Storage**: N/A (in-memory Assembly data structure)  
**Testing**: GTest with `gladius_tests` namespace  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single desktop application  
**Performance Goals**: <5 seconds for deduplication of 100 functions  
**Constraints**: Must not modify file on disk until user saves  
**Scale/Scope**: Typical assemblies have 5-50 functions; edge case up to 100+

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Use `std::unique_ptr`, `std::optional`, STL algorithms |
| II. Test-First Development | ✅ PASS | Unit tests for comparison and deduplication required by spec |
| III. Simplicity First (KISS, DRY, YAGNI) | ✅ PASS | Single-purpose comparison class, reuse existing visitor pattern |
| IV. Consistent Code Style | ✅ PASS | Follow existing `gladius/src/nodes/` conventions |
| V. Documentation and Comments | ✅ PASS | Document public API with Doxygen |

**Pre-design Gate**: ✅ PASSED - No violations identified.

## Project Structure

### Documentation (this feature)

```text
specs/014-dedupe-functions/
├── plan.md              # This file
├── research.md          # Phase 0: Algorithm design decisions
├── data-model.md        # Phase 1: Data structures and interfaces
├── quickstart.md        # Phase 1: Quick implementation guide
├── contracts/           # Phase 1: API contracts
└── tasks.md             # Phase 2: Implementation tasks (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/src/nodes/
├── FunctionalEquality.h         # NEW: Comparison algorithm interface
├── FunctionalEquality.cpp       # NEW: Comparison implementation
├── FunctionDeduplicator.h       # NEW: Deduplication orchestration
├── FunctionDeduplicator.cpp     # NEW: Deduplication implementation
├── Assembly.h                   # MODIFY: Add deduplication method
├── Model.h                      # EXISTING: Core data structure
└── DerivedNodes.h               # EXISTING: FunctionCall node

gladius/src/ui/
└── ModelEditor.cpp              # MODIFY: Add deduplicate button to outline menu

gladius/tests/unittests/
├── FunctionalEquality_tests.cpp # NEW: Comparison algorithm tests
└── FunctionDeduplicator_tests.cpp # NEW: Deduplication tests
```

**Structure Decision**: New files placed in `gladius/src/nodes/` alongside existing `Model.h` and `Assembly.h` since the feature operates on the node graph layer. Tests follow existing naming convention in `gladius/tests/unittests/`.

## Complexity Tracking

> No Constitution violations requiring justification.
