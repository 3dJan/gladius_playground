# Tasks: Surface-Aligned Color Sampling for Shell Generation

**Input**: Design documents from `/specs/011-surface-color-shells/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅, quickstart.md ✅

**Tests**: Unit tests and integration tests are included per plan.md requirements for test-first development.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create new class skeleton and configure build

- [X] T001 Create SurfaceThicknessField header file in gladius/src/io/3mf/SurfaceThicknessField.h
- [X] T002 [P] Create SurfaceThicknessField implementation file in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T003 [P] Add SurfaceThicknessField to CMakeLists.txt in gladius/src/CMakeLists.txt
- [X] T004 [P] Create unit test file in gladius/tests/unittests/SurfaceThicknessField_tests.cpp

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core SurfaceThicknessField class implementation - MUST be complete before any user story

**⚠️ CRITICAL**: No shell generation modification can begin until this phase is complete

- [X] T005 Implement SurfaceThicknessFieldConfig struct in gladius/src/io/3mf/SurfaceThicknessField.h
- [X] T006 Implement SurfaceThicknessField::build() method signature and validation in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T007 Implement SurfaceThicknessField::lookupThickness() trilinear LUT interpolation in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T008 Implement SurfaceThicknessField::rasterizeSurfaceVertices() for vertex-to-grid seeding in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T009 Implement SurfaceThicknessField::propagateInward() BFS flood fill in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T010 Implement SurfaceThicknessField::sampleAt() CPU-side trilinear sampling in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T011 [P] Implement world-to-grid transform computation in SurfaceThicknessField::build() in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [X] T012 [P] Implement getFieldBuffer(), getResolution(), getWorldToGridTransform() getters in gladius/src/io/3mf/SurfaceThicknessField.cpp

**Checkpoint**: SurfaceThicknessField class is fully functional - GPU integration can now begin ✅

---

## Phase 3: User Story 1 - Accurate Color Reproduction on 3D Surfaces (Priority: P1) 🎯 MVP

**Goal**: Fix shell thickness computation to use surface colors (SDF=0) instead of interior colors

**Independent Test**: Export a model with an image projected onto a curved surface. Verify shell thickness at each point corresponds to the surface color, not interior color.

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before GPU kernel implementation**

- [X] T013 [P] [US1] Unit test Build_WithValidInput_Succeeds in gladius/tests/unittests/SurfaceThicknessField_tests.cpp
- [X] T014 [P] [US1] Unit test Build_WithMismatchedArrays_Throws in gladius/tests/unittests/SurfaceThicknessField_tests.cpp
- [X] T015 [P] [US1] Unit test SampleAt_ReturnsInterpolatedValues in gladius/tests/unittests/SurfaceThicknessField_tests.cpp
- [X] T016 [P] [US1] Unit test LookupThickness_TrilinearInterpolation in gladius/tests/unittests/SurfaceThicknessField_tests.cpp
- [X] T017 [P] [US1] Unit test PropagateInward_FillsUnassignedVoxels in gladius/tests/unittests/SurfaceThicknessField_tests.cpp
- [X] T017a [US1] Unit test ThicknessAccuracy_FlatSurface_Exceeds95Percent in gladius/tests/unittests/SurfaceThicknessField_tests.cpp (validates SC-001)

### Implementation for User Story 1

- [X] T018 [US1] Add sampleCornersWithThicknessField kernel to gladius/src/kernel/dual_contouring_sampling.cl (already exists)
- [X] T019 [US1] Add sampleThicknessField helper function (trilinear) to gladius/src/kernel/dual_contouring_sampling.cl (already exists)
- [X] T020 [US1] Add transformToGrid helper function to gladius/src/kernel/dual_contouring_sampling.cl (uses worldToThicknessField matrix)
- [X] T021 [US1] Add thickness field buffer and transform parameters to ManifoldDualContouringGpu in gladius/src/compute/ManifoldDualContouringGpu.cpp (already wired through DualContouringGpuSampling)
- [X] T022 [US1] Modify ShellGenerator to add generateShellsWithSurfaceSampling() method in gladius/src/io/3mf/ShellGenerator.cpp
- [X] T023 [US1] Add useSurfaceColorSampling parameter to generateShells() in gladius/src/io/3mf/ShellGenerator.h
- [X] T024 [US1] Integrate surface mesh extraction (Phase 1) in generateShellsWithSurfaceSampling() in gladius/src/io/3mf/ShellGenerator.cpp
- [X] T025 [US1] Integrate FaceColorSampler vertex sampling (Phase 2) in generateShellsWithSurfaceSampling() in gladius/src/io/3mf/ShellGenerator.cpp
- [X] T026 [US1] Integrate SurfaceThicknessField build (Phase 3) in generateShellsWithSurfaceSampling() in gladius/src/io/3mf/ShellGenerator.cpp
- [X] T027 [US1] Wire thickness field to HierarchicalOctreeBuilder via HierarchicalConfig in gladius/src/HierarchicalDualContouring.h (already exists)
- [X] T027a [US1] Modify HierarchicalOctreeBuilder to sample from SurfaceThicknessField when provided in gladius/src/HierarchicalDualContouring.cpp (already exists)
- [X] T028 [US1] Integration test GenerateShells_WithImageProjection_UsesSurfaceColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T028a [P] [US1] Integration test GenerateShells_PlanarProjection_UsesSurfaceColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T028b [P] [US1] Integration test GenerateShells_TriplanarMapping_UsesSurfaceColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T028c [P] [US1] Integration test GenerateShells_ProceduralTexture_UsesSurfaceColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp

**Checkpoint**: User Story 1 core implementation complete ✅ - shell export uses surface colors for shell thickness sampling

---

## Phase 4: User Story 2 - Consistent Results Across Surface Geometries (Priority: P2)

**Goal**: Ensure correct color reproduction on flat, convex, and concave surfaces

**Independent Test**: Test shell export on flat plane, convex dome, and concave bowl with same projected image. Verify consistent color reproduction across all geometries.

### Tests for User Story 2

- [ ] T029 [P] [US2] Integration test GenerateShells_FlatSurface_MatchesImageColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T030 [P] [US2] Integration test GenerateShells_ConvexDome_MatchesImageColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T031 [P] [US2] Integration test GenerateShells_ConcaveBowl_MatchesImageColors in gladius/tests/integrationtests/ShellGenerator_tests.cpp

### Implementation for User Story 2

- [ ] T032 [US2] Handle edge case: sharp edges with discontinuous normals in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [ ] T033 [US2] Handle edge case: high curvature regions with dense vertex sampling in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [ ] T034 [US2] Add warning log for unmapped color fallback in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [ ] T035 [US2] Verify mesh watertightness for all shell outputs in gladius/tests/integrationtests/ShellGenerator_tests.cpp

**Checkpoint**: User Stories 1 AND 2 complete - shell export works on all surface types

---

## Phase 5: User Story 3 - Performance Within Acceptable Bounds (Priority: P3)

**Goal**: Shell export completes within 3x baseline time for 100k triangle equivalent models

**Independent Test**: Time shell export for reference model and compare to baseline. Performance should be within 3x of current implementation.

### Tests for User Story 3

- [ ] T036 [P] [US3] Performance test ShellExport_100kTriangles_Within3xBaseline in gladius/tests/integrationtests/ShellGenerator_tests.cpp
- [ ] T037 [P] [US3] Test ProgressReporting_IsCancellable in gladius/tests/integrationtests/ShellGenerator_tests.cpp

### Implementation for User Story 3

- [ ] T038 [US3] Add progress reporting callbacks to generateShellsWithSurfaceSampling() in gladius/src/io/3mf/ShellGenerator.cpp
- [ ] T039 [US3] Ensure cancellation check during SurfaceThicknessField::build() in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [ ] T040 [US3] Profile and optimize propagateInward() if needed in gladius/src/io/3mf/SurfaceThicknessField.cpp
- [ ] T041 [US3] Add configurable grid resolution based on model complexity in gladius/src/io/3mf/ShellGenerator.cpp

**Checkpoint**: All user stories complete - feature is correct, robust, and performant

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, cleanup, and validation

- [X] T042 [P] Add Doxygen comments to SurfaceThicknessField public API in gladius/src/io/3mf/SurfaceThicknessField.h (already documented)
- [X] T043 [P] Update ShellGenerator Doxygen with new useSurfaceColorSampling parameter in gladius/src/io/3mf/ShellGenerator.h (already documented)
- [X] T044 [P] Add inline comments explaining GPU kernel trilinear sampling in gladius/src/kernel/dual_contouring_sampling.cl (already documented)
- [X] T045 Run all existing shell generation tests to verify no regressions (38 tests pass)
- [X] T046 Run quickstart.md validation - verify documented API matches implementation (updated to match)
- [X] T047 Update MCP_IMPLEMENTATION.md if shell export MCP endpoints affected in gladius/MCP_IMPLEMENTATION.md (N/A - no shell endpoints in MCP)

**Checkpoint**: Phase 6 complete ✅

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational - core bug fix
- **User Story 2 (Phase 4)**: Depends on US1 - geometry robustness
- **User Story 3 (Phase 5)**: Depends on US1 - performance validation
- **Polish (Phase 6)**: Depends on all user stories being complete

### User Story Dependencies

```
Setup (T001-T004)
    │
    ▼
Foundational (T005-T012) ──BLOCKS ALL──▶ User Stories
    │
    ├──▶ US1: P1 (T013-T028) ──▶ Core fix
    │         │
    │         ├──▶ US2: P2 (T029-T035) ──▶ Geometry robustness
    │         │
    │         └──▶ US3: P3 (T036-T041) ──▶ Performance
    │
    └──▶ Polish (T042-T047)
```

### Within Each Phase

- Tests MUST be written and FAIL before implementation (TDD)
- Class structure before methods
- Core methods before integration
- Phase complete before moving to next

### Parallel Opportunities

**Phase 1 (Setup)**:
- T002, T003, T004 can run in parallel after T001

**Phase 2 (Foundational)**:
- T011, T012 can run in parallel after T010

**Phase 3 (US1)**:
- T013-T017 (all unit tests) can run in parallel
- T018-T020 (GPU kernel parts) are sequential but T021 can start after T020

**Phase 4 (US2)**:
- T029-T031 (integration tests) can run in parallel

**Phase 5 (US3)**:
- T036, T037 (tests) can run in parallel

**Phase 6 (Polish)**:
- T042, T043, T044 can run in parallel

---

## Parallel Example: User Story 1 Tests

```bash
# Launch all unit tests for US1 together:
Task: "Unit test Build_WithValidInput_Succeeds in gladius/tests/unittests/SurfaceThicknessField_tests.cpp"
Task: "Unit test Build_WithMismatchedArrays_Throws in gladius/tests/unittests/SurfaceThicknessField_tests.cpp"
Task: "Unit test SampleAt_ReturnsInterpolatedValues in gladius/tests/unittests/SurfaceThicknessField_tests.cpp"
Task: "Unit test LookupThickness_TrilinearInterpolation in gladius/tests/unittests/SurfaceThicknessField_tests.cpp"
Task: "Unit test PropagateInward_FillsUnassignedVoxels in gladius/tests/unittests/SurfaceThicknessField_tests.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (4 tasks, ~1h)
2. Complete Phase 2: Foundational (8 tasks, ~4h)
3. Complete Phase 3: User Story 1 (16 tasks, ~8h)
4. **STOP and VALIDATE**: Test with image projection on curved surface
5. Deploy/demo if ready - core bug is fixed

### Incremental Delivery

1. Setup + Foundational → SurfaceThicknessField class ready
2. Add User Story 1 → Core fix deployed (MVP!)
3. Add User Story 2 → Geometry robustness validated
4. Add User Story 3 → Performance confirmed
5. Each story adds confidence without breaking previous functionality

---

## Summary

| Phase | Tasks | Effort | Description |
|-------|-------|--------|-------------|
| Setup | T001-T004 | ~1h | File creation, CMake |
| Foundational | T005-T012 | ~4h | SurfaceThicknessField implementation |
| US1 (P1) | T013-T028c | ~9h | Core bug fix, GPU kernel, integration, color method tests |
| US2 (P2) | T029-T035 | ~4h | Geometry robustness, edge cases |
| US3 (P3) | T036-T041 | ~3h | Performance, progress reporting |
| Polish | T042-T047 | ~1h | Documentation, validation |
| **Total** | **52 tasks** | **~22h** | |

---

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable
- Verify tests fail before implementing (TDD approach per constitution)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Grid resolution default: 128³ (~8MB) - configurable for large models
