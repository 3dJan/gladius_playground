# Tasks: Fast Mesh Simplification for Export

**Input**: Design documents from `/specs/026-fast-mesh-simplification/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/internal-api.md, quickstart.md

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1–US5)
- All paths are relative to repository root

---

## Phase 1: Setup (Shared Type Definitions)

**Purpose**: Extend existing enums and add new config types that all user stories depend on

- [X] T001 [P] Add QemFast value to io::SimplificationMethod enum, add SimplificationTerminationMode enum, and add simplificationMaxError field in gladius/src/io/SurfaceExtractionOptions.h
- [X] T002 [P] Add QemFast value to compute::SimplificationMethod enum in gladius/src/compute/ManifoldDualContouringGpu.h
- [X] T003 [P] Add compute::SimplificationTerminationMode enum, FastQemConfig struct, and fastQemSimplify() function declaration in gladius/src/compute/MeshSimplification.h

**Checkpoint**: All enums and config types compile. No behavioral changes yet.

---

## Phase 2: Foundational (Core Data Structures)

**Purpose**: Implement reusable data structures required by the fast QEM algorithm

**⚠️ CRITICAL**: The algorithm implementation in Phase 3 cannot begin until these are complete

- [X] T004 Implement SymMat double-precision 4×4 symmetric matrix class (construct from plane, add, determinant, evaluate error) in gladius/src/compute/MeshSimplification.h
- [X] T005 Implement MutablePriorityQueue template class (push, pop, update, remove with O(log n) operations and IndexSetter callback) in gladius/src/compute/MeshSimplification.h

**Checkpoint**: Data structures compile and are ready for use. Consider adding standalone unit tests for SymMat arithmetic and MutablePriorityQueue operations.

---

## Phase 3: User Story 1 + User Story 2 — Fast Simplification + Printable Output (Priority: P1) 🎯 MVP

**Goal**: Implement the core fast single-pass greedy QEM algorithm that produces valid, printable meshes. US1 (speed) and US2 (quality/printability) are inseparable — the topology guards ARE the algorithm.

**Independent Test**: Export a 1M-triangle model with 50% reduction using each extraction method. Verify: completes in <10s, correct triangle count, passes manifold/watertight validation, no flipped normals.

### Implementation

- [X] T006 [US1] Implement adjacency data structures (VertexInfo, TriangleInfo, EdgeInfo) and initialization routines: quadric computation from triangle planes, flat adjacency array build, initial per-edge error evaluation with optimal vertex placement in gladius/src/compute/MeshSimplification.cpp
- [X] T007 [US1] [US2] Implement core simplification loop: priority-queue-driven greedy edge collapse with topology guards (normal flip detection via dot threshold, manifold edge check, boundary edge preservation), neighbor adjacency updates, quadric accumulation, and priority queue updates in gladius/src/compute/MeshSimplification.cpp
- [X] T008 [US1] Implement mesh compaction pass: remove deleted vertices and triangles, reindex triangle indices, shrink position and index vectors in gladius/src/compute/MeshSimplification.cpp
- [X] T009 [P] [US1] Wire up QemFast case in ManifoldDualContouringGpu::simplifyMesh() switch statement to call fastQemSimplify() in gladius/src/compute/ManifoldDualContouringGpu.cpp
- [X] T010 [P] [US1] Map QemFast io::SimplificationMethod to compute::FastQemConfig in the exporter, ensuring simplification runs for all extraction methods (LMC, DC, MDC) in gladius/src/io/ManifoldDualContouringStlExporter.cpp
- [X] T011 [US1] [US2] Add unit tests for fast QEM: basic sphere reduction to target count, reduction percentage mode, comparative timing benchmark (fast mode ≥5x faster than SDF-aware mode on same mesh), manifold preservation after 50%/75% reduction, no normal flips, watertight output validation in gladius/tests/unittests/MeshSimplification_tests.cpp
- [X] T011b [US1] Add regression test for existing QemSdfAware simplification: verify it still produces valid manifold output after enum and switch-statement changes in gladius/tests/unittests/MeshSimplification_tests.cpp

**Checkpoint**: Fast geometric simplification works end-to-end. Existing SDF-aware mode is verified unbroken. Meshes can be exported with "Fast (Geometric)" mode and loaded in a slicer without errors.

---

## Phase 4: User Story 3 — Color Preservation After Simplification (Priority: P2)

**Goal**: Ensure per-vertex colors are correctly resampled from the implicit color function after fast QEM simplification, using the existing color evaluation pipeline.

**Independent Test**: Export a colored model with simplification enabled. Verify per-vertex colors match the implicit color function at the new vertex positions within tolerance.

### Implementation

- [X] T012 [US3] Verify and adjust color resampling pipeline ordering: ensure color evaluation executes AFTER fast QEM simplification (not before) for the QemFast path in gladius/src/io/ManifoldDualContouringStlExporter.cpp
- [X] T013 [US3] Add unit test for color preservation: simplify a colored mesh, verify colors at new vertex positions match expected values from the color function within 5% tolerance in gladius/tests/unittests/MeshSimplification_tests.cpp

**Checkpoint**: Colored models export correctly with simplification. Colors are visually consistent with the unsimplified version.

---

## Phase 5: User Story 4 — User Controls for Simplification (Priority: P2)

**Goal**: Expose algorithm selection (fast vs SDF-aware) and termination mode (triangle count, reduction %, error-bounded) in the export dialog.

**Independent Test**: Open export dialog, verify all new options are present. Export with each termination mode and verify the output meets the specified target.

### Implementation

- [X] T014 [P] [US4] Add UI state members for termination mode selection and max error threshold in gladius/src/ui/MeshExportDialog.h
- [X] T015 [US4] Add "Fast (Geometric)" to simplification method combo, add termination mode dropdown (Target Count / Reduction % / Error-Bounded), and conditionally show max error input field in gladius/src/ui/MeshExportDialog.cpp
- [X] T016 [US4] Add unit test for error-bounded termination mode: verify simplification stops when all remaining edge collapses exceed the configured error threshold in gladius/tests/unittests/MeshSimplification_tests.cpp

**Checkpoint**: Export dialog shows all simplification options. Each termination mode produces the expected result.

---

## Phase 6: User Story 5 — Progress Feedback During Simplification (Priority: P3)

**Goal**: Display a progress indicator during simplification and support cancellation for large meshes.

**Independent Test**: Export a 2M+ triangle model with simplification. Verify progress updates appear and cancellation stops the operation cleanly.

### Implementation

- [X] T017 [US5] Implement periodic progress reporting (collapses_done / total_collapses as 0–100%) and cancellation check (every cancelCheckPeriod collapses) in the fastQemSimplify() main loop in gladius/src/compute/MeshSimplification.cpp
- [X] T018 [US5] Add unit test for cancellation: trigger cancel callback during simplification, verify it throws and the mesh is in a valid (partially simplified) state in gladius/tests/unittests/MeshSimplification_tests.cpp

**Checkpoint**: Progress indicator updates during long simplification runs. Cancel stops the operation within 1 second.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, validation, and cleanup

- [X] T019 [P] Add Doxygen documentation for all public API: fastQemSimplify(), FastQemConfig, MutablePriorityQueue, SymMat in gladius/src/compute/MeshSimplification.h
- [X] T020 Run end-to-end quickstart.md validation: build, run tests, manual export test with each extraction method and both simplification modes

**Checkpoint**: Feature is complete, documented, and validated.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on T003 (FastQemConfig declaration) — BLOCKS all algorithm work
- **US1+US2 (Phase 3)**: Depends on Phase 2 completion — core algorithm implementation
- **US3 (Phase 4)**: Depends on Phase 3 (simplification must work before color can be verified)
- **US4 (Phase 5)**: Depends on Phase 1 (enums) and Phase 3 (algorithm working) — UI wiring
- **US5 (Phase 6)**: Depends on Phase 3 T007 (main loop exists to add progress/cancel to)
- **Polish (Phase 7)**: Depends on all previous phases

### Within Phase 3 (Critical Path)

```
T006 (data structures + init)
  → T007 (core loop + topology guards)
    → T008 (compaction)
      → T009 [P] (wire up in MDC)
      → T010 [P] (map in exporter)
        → T011 (tests)
```

### User Story Independence

- **US1 + US2**: Combined because topology guards (US2) are integral to the collapse loop (US1)
- **US3**: Independent — only verifies pipeline ordering and adds a test
- **US4**: Independent — UI changes only, reads existing enums from Phase 1
- **US5**: Independent — adds callbacks to existing loop from Phase 3

### Parallel Opportunities

**Phase 1** (all three tasks in parallel — different files):
```
T001 (SurfaceExtractionOptions.h)  ║  T002 (ManifoldDualContouringGpu.h)  ║  T003 (MeshSimplification.h)
```

**Phase 3** (after T008, two tasks in parallel — different files):
```
T009 (ManifoldDualContouringGpu.cpp)  ║  T010 (ManifoldDualContouringStlExporter.cpp)
```

**Phase 4 + Phase 5** (independent stories, can run in parallel after Phase 3):
```
T012–T013 (US3: color)  ║  T014–T016 (US4: UI controls)  ║  T017–T018 (US5: progress)
```

---

## Implementation Strategy

### MVP First (Phase 1 + 2 + 3 Only)

1. Complete Phase 1: Setup — add enums and types
2. Complete Phase 2: Foundational — SymMat + MutablePriorityQueue
3. Complete Phase 3: US1 + US2 — core algorithm + tests
4. **STOP and VALIDATE**: Export meshes with all 3 extraction methods, verify in slicer
5. This delivers the core value: fast, printable mesh simplification

### Incremental Delivery

1. Setup + Foundational → types ready
2. US1 + US2 → fast simplification works, meshes are printable → **MVP!**
3. US3 → colors preserved after simplification
4. US4 → full user control via export dialog
5. US5 → progress indicator for large meshes
6. Polish → documentation and validation

---

## Notes

- All 9 modified files are existing — no new source files needed
- The fast QEM algorithm is ~600–800 lines in MeshSimplification.cpp alongside the existing SDF-aware simplifier
- SymMat uses double precision internally but float for vertex positions (research R3)
- MutablePriorityQueue follows PrusaSlicer's IndexSetter callback pattern (research R4)
- Color resampling reuses the existing implicit color evaluation pipeline — no new color code needed (research R5)
- Topology guards (flip detection threshold 0.2, boundary preservation) are based on PrusaSlicer's proven defaults (research R1)
