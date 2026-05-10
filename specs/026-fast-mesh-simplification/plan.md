# Implementation Plan: Fast Mesh Simplification for Export

**Branch**: `026-fast-mesh-simplification` | **Date**: 2026-03-26 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/026-fast-mesh-simplification/spec.md`

## Summary

Replace the current multi-pass batch QEM mesh simplification with a fast single-pass greedy QEM algorithm using an incremental mutable priority queue (modelled after PrusaSlicer/OrcaSlicer's approach). The new "fast (geometric)" mode operates purely on CPU without GPU SDF evaluation, achieving >5x speedup. The existing SDF-aware mode is retained. Color resampling is performed after simplification by querying the implicit color function at new vertex positions. The export dialog is extended with algorithm selection and termination mode controls.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: Eigen (linear algebra), ImGui (UI), lib3mf (export), GTest/GMock (testing)  
**Storage**: N/A (in-memory mesh processing)  
**Testing**: GTest/GMock, existing test at `gladius/tests/unittests/MeshSimplification_tests.cpp`  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single C++ project (gladius/)  
**Performance Goals**: 1M-triangle mesh simplified to 50% in <10 seconds; fast mode ≥5x faster than SDF-aware mode  
**Constraints**: Must preserve mesh manifoldness and watertightness; must work with all 3 extraction methods  
**Scale/Scope**: Core algorithm ~600-800 lines, UI changes ~50 lines, test additions ~200 lines

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | C++20, STL containers, smart pointers, const correctness, move semantics |
| II. Test-First Development | ✅ PASS | Existing test file will be extended; new algorithm gets dedicated tests |
| III. Simplicity First (KISS/DRY/YAGNI) | ✅ PASS | Single-pass algorithm is simpler than current multi-pass approach; minimal new abstractions |
| IV. Consistent Code Style | ✅ PASS | Allman braces, camelCase, PascalCase types, m_ prefix for members |
| V. Documentation and Comments | ✅ PASS | Public API documented with Doxygen; algorithm references in header comment |
| VI. UI Responsiveness | ✅ PASS | Simplification runs on export worker thread (already async); progress callback + cancellation support |

**Gate result: PASS** — No violations. All principles are satisfied by design.

## Project Structure

### Documentation (this feature)

```text
specs/026-fast-mesh-simplification/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (internal API contracts)
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/src/
├── compute/
│   ├── MeshSimplification.h          # MODIFY: Add FastQemConfig, fastQemSimplify(), MutablePriorityQueue
│   ├── MeshSimplification.cpp        # MODIFY: Implement fast single-pass QEM algorithm
│   ├── ManifoldDualContouringGpu.h   # MODIFY: Add SimplificationMethod::QemFast enum value
│   └── ManifoldDualContouringGpu.cpp # MODIFY: Wire up fast simplification path
├── io/
│   ├── SurfaceExtractionOptions.h    # MODIFY: Add QemFast enum value, termination mode enum
│   └── ManifoldDualContouringStlExporter.cpp # MODIFY: Map new options
└── ui/
    ├── MeshExportDialog.h            # MODIFY: Add UI state for new controls
    └── MeshExportDialog.cpp          # MODIFY: Add fast/quality toggle, termination mode controls

gladius/tests/unittests/
└── MeshSimplification_tests.cpp      # MODIFY: Add tests for fast QEM algorithm
```

**Structure Decision**: All changes fit within existing file structure. The fast QEM algorithm is added alongside the existing SDF-aware simplifier in `MeshSimplification.h/.cpp`. No new files needed — this keeps the codebase cohesive and avoids file sprawl.
