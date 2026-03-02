# Tasks: MDC Export Progress Indication

**Input**: Design documents from `/specs/001-mdc-export-progress/`  
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/progress-callback.md ✅

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Paths are relative to `gladius/` directory

---

## Phase 1: Setup

**Purpose**: Define the callback type and extend existing infrastructure

- [x] T001 Define `MeshGenerationProgressCallback` type alias in `src/compute/ManifoldDualContouringGpu.h`
- [x] T002 [P] Add `m_progressCallback` member and `setProgressCallback()` method declaration to `ManifoldDualContouringGpu` class in `src/compute/ManifoldDualContouringGpu.h`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core callback infrastructure that all user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T003 Implement `setProgressCallback()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T004 Add helper method `reportProgress(float localProgress, std::string_view phase)` to convert local phase progress to global [0,1] range in `src/compute/ManifoldDualContouringGpu.cpp`

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Async Export with Live Progress (Priority: P1) 🎯 MVP

**Goal**: Progress bar updates smoothly in real-time during MDC export

**Independent Test**: Initiate MDC export on complex model, observe progress bar updates incrementally (not jumping 0%→25%→70%→100%)

### Implementation for User Story 1

- [x] T005 [US1] Add progress callback invocations in `constructOctree()` method (per-depth level) in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T006 [US1] Add progress callback invocations in `generateVertices()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T007 [US1] Add progress callback invocations in `generateIndices()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T008 [US1] Add progress callback invocations in `generateMeshNonHierarchical()` for chunked mode (per-chunk) in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T009 [US1] Add progress callback invocations in `generateMeshHierarchical()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T010 [US1] Wire progress callback in `ManifoldDualContouringStlExporter::performExport()` - create lambda that updates `m_progress` atomically in `src/io/ManifoldDualContouringStlExporter.cpp`
- [x] T011 [US1] Update progress phase allocation in exporter to match data-model.md ranges (0-5% init, 5-65% mesh gen, etc.) in `src/io/ManifoldDualContouringStlExporter.cpp`

**Checkpoint**: User Story 1 complete - progress bar should update smoothly during export

---

## Phase 4: User Story 2 - Export Remains Non-Blocking (Priority: P1)

**Goal**: UI remains responsive during export; export already runs async

**Independent Test**: Start export, verify viewport rotation/zoom works without lag

### Implementation for User Story 2

- [x] T012 [US2] Verify async infrastructure: Run MDC export on complex model, confirm viewport rotation/zoom works during export, verify no UI freeze in `src/io/ManifoldDualContouringStlExporter.cpp`
- [x] T013 [US2] Add Doxygen documentation for thread-safety guarantees of progress callback in `src/compute/ManifoldDualContouringGpu.h`

> **Note**: Cancel functionality (FR-004) is already implemented via `onExportCancelled()` in `MeshExportDialog`. No new tasks required.

**Checkpoint**: User Story 2 verified - async behavior unchanged

---

## Phase 5: User Story 3 - Progress Bar Reflects Export Phases (Priority: P2)

**Goal**: Progress bar reflects distinct phases (mesh generation, post-processing, file writing)

**Independent Test**: Observe progress advancing through distinguishable phases without stalling

### Implementation for User Story 3

- [x] T014 [US3] Add progress callback invocations in `postProcessSharpFeatures()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T015 [US3] Add progress callback invocations in `simplifyMesh()` / `simplifyMeshQemSdfAware()` methods in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T016 [US3] Add progress callback invocations in `improveMeshQuality()` method in `src/compute/ManifoldDualContouringGpu.cpp`
- [x] T017 [US3] Add progress updates during color sampling phase in `writeMeshToFile()` for 3MF export in `src/io/ManifoldDualContouringStlExporter.cpp`
- [x] T018 [US3] Add progress updates during STL/3MF file writing phase in `src/io/ManifoldDualContouringStlExporter.cpp`
- [ ] T018a [US3] Wire phase name from progress callback to UI status message in `src/ui/MeshExportDialog.cpp` (FR-005) - **Deferred: Optional enhancement**

**Checkpoint**: User Story 3 complete - all phases report progress with status messages

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Tests, documentation, and validation

- [x] T019 [P] Create unit test file `tests/unittests/ManifoldDualContouringProgress_tests.cpp`
- [x] T020 [P] Add test: `ManifoldDualContouringGpu_SetProgressCallback_CallbackIsInvoked` in `tests/unittests/ManifoldDualContouringProgress_tests.cpp`
- [x] T021 [P] Add test: `ManifoldDualContouringGpu_GenerateMesh_ProgressIsMonotonic` in `tests/unittests/ManifoldDualContouringProgress_tests.cpp`
- [x] T022 [P] Add test: `ManifoldDualContouringGpu_GenerateMesh_ProgressReaches100Percent` in `tests/unittests/ManifoldDualContouringProgress_tests.cpp`
- [x] T023 Add `ManifoldDualContouringProgress_tests.cpp` to `tests/unittests/CMakeLists.txt` (auto-discovered via glob)
- [ ] T024 Run quickstart.md validation to confirm feature works end-to-end

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup) → Phase 2 (Foundational) → Phase 3-5 (User Stories) → Phase 6 (Polish)
```

### User Story Dependencies

- **User Story 1 (P1)**: Depends on Phase 2 - core progress callback infrastructure
- **User Story 2 (P1)**: No implementation changes, only verification
- **User Story 3 (P2)**: Depends on Phase 2 - builds on US1 infrastructure

### Within Each User Story

- T005-T009 can run in parallel (different methods)
- T010-T011 depend on T005-T009 (exporter wires what compute provides)
- T014-T016 can run in parallel (different post-processing methods)
- T019-T022 can run in parallel (different test cases)

### Parallel Opportunities

```bash
# After Phase 2 completes, these can run in parallel:
T005, T006, T007  # Different methods in same file (careful with conflicts)

# Test cases can run in parallel:
T019, T020, T021, T022  # Different test cases in same file
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T002)
2. Complete Phase 2: Foundational (T003-T004)
3. Complete Phase 3: User Story 1 (T005-T011)
4. **VALIDATE**: Run export on complex model, verify progress updates
5. ✅ MVP Complete - core functionality working

### Full Implementation

1. MVP (above)
2. Phase 4: User Story 2 verification (T012-T013)
3. Phase 5: User Story 3 phase reporting (T014-T018)
4. Phase 6: Tests and polish (T019-T024)

---

## Summary

| Category | Count | Completed |
|----------|-------|-----------|
| Total Tasks | 25 | 23 |
| Setup Tasks | 2 | 2 |
| Foundational Tasks | 2 | 2 |
| User Story 1 Tasks | 7 | 7 |
| User Story 2 Tasks | 2 | 2 |
| User Story 3 Tasks | 6 | 5 |
| Polish Tasks | 6 | 5 |
| Parallel Opportunities | T019-T022 (tests) | All complete |

**MVP Scope**: Tasks T001-T011 (11 tasks) delivers core progress indication functionality. ✅ **COMPLETE**

**Remaining**: 
- T018a: Wire phase name to UI (optional enhancement, deferred)
- T024: Manual quickstart.md validation
