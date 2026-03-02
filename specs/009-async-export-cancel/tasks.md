# Tasks: Async Export Cancellation

**Input**: Design documents from `/specs/009-async-export-cancel/`  
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, quickstart.md ✅

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)

## Path Conventions

- **Source**: `gladius/src/`
- **Unit tests**: `gladius/tests/unittests/`
- **Integration tests**: `gladius/tests/integrationtests/`

---

## Phase 1: Setup

**Purpose**: Create the core CancellationToken infrastructure

- [x] T001 [P] Create `CancellationToken` class in gladius/src/io/CancellationToken.h
- [x] T002 [P] Add unit tests for CancellationToken in gladius/tests/unittests/CancellationToken_tests.cpp
- [x] T003 Register CancellationToken_tests.cpp in gladius/tests/unittests/CMakeLists.txt (auto-registered via GLOB_RECURSE)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Extend core infrastructure that all user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T004 Add `ExportPhase` enum (Idle, Exporting, Cancelling) to gladius/src/ui/ExportState.h
- [x] T005 Add `m_phase` atomic member and phase-related methods to ExportState class in gladius/src/ui/ExportState.h
- [x] T006 Add unit tests for ExportState Cancelling phase in gladius/tests/unittests/ExportState_tests.cpp
- [x] T007 Add `setCancellationToken()` and `isCancellationRequested()` to IExporter interface in gladius/src/io/IExporter.h

**Checkpoint**: Foundation ready - user story implementation can now begin ✅

---

## Phase 3: User Story 1 - Instant Cancel Feedback (Priority: P1) 🎯 MVP

**Goal**: Cancel button immediately shows "Cancelling..." state without blocking UI

**Independent Test**: Click Cancel during export → UI shows "Cancelling..." within one frame, no freeze

### Implementation for User Story 1

- [x] T008 [US1] Add `m_cancellationToken` member to MeshExportDialog in gladius/src/ui/MeshExportDialog.h
- [x] T009 [US1] Update `onExportCancelled()` to be non-blocking in gladius/src/ui/MeshExportDialog.cpp:
  - Set ExportState to Cancelling phase
  - Set status message to "Cancelling..."
  - Call `m_cancellationToken.requestCancellation()`
  - Do NOT call `finalize()` synchronously
- [x] T010 [US1] Update Cancel button rendering in `render()` to show "Cancelling..." disabled button when `isCancelling()` in gladius/src/ui/MeshExportDialog.cpp

**Checkpoint**: Cancel click immediately updates UI - no blocking ✅

---

## Phase 4: User Story 2 - Fast Export Abort (Priority: P1)

**Goal**: Export worker stops within 2 seconds of cancel request

**Independent Test**: Start large mesh export, cancel, measure time until advanceExport returns false

### Implementation for User Story 2

- [x] T011 [P] [US2] Add `m_cancellationToken` member to ManifoldDualContouringStlExporter in gladius/src/io/ManifoldDualContouringStlExporter.h (via IExporter base class)
- [x] T012 [P] [US2] Add `m_cancellationToken` member to DualContouringStlExporter in gladius/src/io/DualContouringStlExporter.h (via IExporter base class)
- [x] T013 [US2] Add cancellation checkpoints in `ManifoldDualContouringStlExporter::performExport()` in gladius/src/io/ManifoldDualContouringStlExporter.cpp:
  - Check after bbox computation
  - Check after mesh generation (main expensive step)
  - Check before file write
- [x] T014 [US2] Add cancellation checkpoints in `DualContouringStlExporter` export loop in gladius/src/io/DualContouringStlExporter.cpp
- [x] T015 [US2] Pass cancellation token to active exporter in `startExport()` in gladius/src/ui/MeshExportDialog.cpp

**Checkpoint**: Export aborts quickly when cancelled ✅

---

## Phase 5: User Story 3 - Non-Blocking Main Thread (Priority: P1)

**Goal**: UI remains responsive (30+ fps) during entire cancellation process

**Independent Test**: Cancel export, immediately rotate 3D viewport → viewport responds smoothly

### Implementation for User Story 3

- [x] T016 [US3] Remove blocking `finalize()` call from `onExportCancelled()` in gladius/src/ui/MeshExportDialog.cpp
- [x] T017 [US3] Update `render()` loop to detect cancellation completion via `advanceExport()` returning false in gladius/src/ui/MeshExportDialog.cpp
- [x] T018 [US3] Call `finalize()` only after worker thread exits (non-blocking path) in gladius/src/ui/MeshExportDialog.cpp

**Checkpoint**: No UI freeze during or after cancel - viewport always responsive ✅

---

## Phase 6: User Story 4 - Clean State After Cancel (Priority: P2)

**Goal**: Partial files deleted, state reset for next export

**Independent Test**: Cancel export, verify no partial file on disk, start new export successfully

### Implementation for User Story 4

- [x] T019 [US4] Add partial file cleanup in render loop after cancelled export detected in gladius/src/ui/MeshExportDialog.cpp:
  - Check if file exists and was not completed
  - Delete partial file via `std::filesystem::remove()`
  - Handle deletion failure: log warning, continue cleanup (FR-008, Edge Case 4)
- [x] T020 [US4] Display "Export cancelled" status message after cancellation completes (FR-008) in gladius/src/ui/MeshExportDialog.cpp
- [x] T021 [US4] Reset export state after cancellation cleanup in gladius/src/ui/MeshExportDialog.cpp:
  - Clear `m_activeExporter`
  - Reset `m_exportInProgress`
  - Call `m_exportState->endExport()`
- [x] T022 [US4] Reset CancellationToken in `startExport()` for reuse in gladius/src/ui/MeshExportDialog.cpp

**Checkpoint**: Clean slate after cancel - new export works immediately ✅

---

## Phase 7: Polish & Verification

**Purpose**: Final validation and edge case handling

- [x] T023 [P] Add integration tests for cancel flow in gladius/tests/integrationtests/ExportCancellation_tests.cpp
- [x] T024 [P] Handle edge case: rapid multiple Cancel clicks (ignore after first) in gladius/src/ui/MeshExportDialog.cpp (button disabled when cancelling)
- [x] T025 [P] Handle edge case: export completes between click and signal (treat as success) in gladius/src/ui/MeshExportDialog.cpp (handled by checking wasCancelled after export finishes)
- [x] T026 Build and run all unit tests (580 passed, 5 skipped)
- [ ] T027 Manual testing: verify UI responsiveness during cancellation with large mesh

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup)
    └── Phase 2 (Foundational) ──┬── Phase 3 (US1: Feedback) ─┐
                                 ├── Phase 4 (US2: Abort)     ├── Phase 7 (Polish)
                                 ├── Phase 5 (US3: Non-Block) │
                                 └── Phase 6 (US4: Cleanup) ──┘
```

- **Phase 1**: No dependencies - can start immediately
- **Phase 2**: Depends on Phase 1 - BLOCKS all user stories
- **Phases 3-6**: All depend on Phase 2, can proceed in parallel (but recommended: 3→4→5→6)
- **Phase 7**: Depends on all user stories complete

### Task Dependencies Within Phases

- T003 depends on T002 (register test after creating it)
- T009, T010 depend on T008 (add member before using it)
- T013 depends on T011 (add member before using it)
- T014 depends on T012 (add member before using it)
- T015 depends on T008, T011, T012 (token and exporters ready)
- T017, T018 depend on T016 (non-blocking cancel path)
- T019, T020, T021 depend on T017 (detect completion first)
- T022 depends on T008 (token member exists)

### Parallel Opportunities

Within each phase, tasks marked [P] can run in parallel:

```bash
# Phase 1 parallel:
T001 + T002 (different files)

# Phase 4 parallel:
T011 + T012 (different exporter files)

# Phase 7 parallel:
T023 + T024 + T025 (different concerns)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T007)
3. Complete Phase 3: User Story 1 (T008-T010)
4. **STOP and VALIDATE**: Cancel shows "Cancelling..." immediately
5. Proceed with remaining stories

### Recommended Order (Sequential)

For single developer, execute in this order:
1. T001 → T002 → T003 (Setup)
2. T004 → T005 → T006 → T007 (Foundational)
3. T008 → T009 → T010 (US1: Instant feedback)
4. T011, T012 → T013 → T014 → T015 (US2: Fast abort)
5. T016 → T017 → T018 (US3: Non-blocking)
6. T019 → T020 → T021 → T022 (US4: Cleanup)
7. T023, T024, T025 → T026 → T027 (Polish)

---

## Notes

- Tests are included (unit tests for CancellationToken and ExportState, integration tests for cancel flow)
- All exporter modifications follow same pattern: add token member, add checkpoint checks
- Key insight: remove blocking `finalize()` from `onExportCancelled()`, let normal completion path handle it
- Build with VS Code task "Build ALL (linux-releaseWithDebug)"
- Run unit tests with VS Code task "Run Unit Tests (Fast)"
