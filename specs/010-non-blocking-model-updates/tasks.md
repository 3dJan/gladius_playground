````markdown
# Tasks: Non-Blocking Model Updates

**Input**: Design documents from `/specs/010-non-blocking-model-updates/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅

**Tests**: Manual testing required; automated unit tests for new state tracking APIs.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `gladius/src/`
- **Tests**: `gladius/tests/unittests/`
- **Compute**: `gladius/src/compute/`
- **UI**: `gladius/src/ui/`

---

## Phase 1: Setup

**Purpose**: No setup needed - this feature extends existing code

✅ No tasks required - project infrastructure already exists.

---

## Phase 2: Foundational (State Tracking Infrastructure)

**Purpose**: Add atomic flags and query methods to ComputeCore that all user stories depend on

**⚠️ CRITICAL**: These state tracking additions MUST be complete before busy indicator can be extended

- [X] T001 Add `m_sdfComputationInProgress` atomic flag to ComputeCore in gladius/src/compute/ComputeCore.h
- [X] T002 Add `m_boundingBoxComputationInProgress` atomic flag to ComputeCore in gladius/src/compute/ComputeCore.h
- [X] T003 [P] Add `isSdfComputationInProgress()` const query method to ComputeCore in gladius/src/compute/ComputeCore.cpp
- [X] T004 [P] Add `isBoundingBoxComputationInProgress()` const query method to ComputeCore in gladius/src/compute/ComputeCore.cpp
- [X] T005 Set `m_sdfComputationInProgress = true` at start of `precomputeSdfAsync()` in gladius/src/compute/ComputeCore.cpp
- [X] T006 Set `m_sdfComputationInProgress = false` at end of SDF computation callback in gladius/src/compute/ComputeCore.cpp
- [X] T007 Set `m_boundingBoxComputationInProgress = true` at start of `updateBoundingBoxFast()` in gladius/src/compute/ComputeCore.cpp
- [X] T008 Set `m_boundingBoxComputationInProgress = false` at end of `updateBoundingBoxFast()` in gladius/src/compute/ComputeCore.cpp
- [ ] T009 Add unit test for state tracking atomics in gladius/tests/unittests/test_ComputeCore.cpp

**Checkpoint**: State tracking infrastructure complete - busy indicator extension can now proceed

---

## Phase 3: User Story 2 - Preview Busy Indicator During Compute (Priority: P1) 🎯 MVP

**Goal**: Show clear busy indicator overlay when any compute operation is in progress

**Independent Test**: Make a graph change triggering recompilation → verify busy indicator appears and stays visible until computation completes

**Note**: Implementing US2 first because US1 (fluid parameter editing) already works via existing `tryToupdateParameter()`. The missing piece is the busy indicator not covering all states, which US2 addresses.

### Implementation for User Story 2

- [X] T010 [US2] Extend busy indicator condition at RenderWindow.cpp:581 to include `isSdfComputationInProgress()` in gladius/src/ui/RenderWindow.cpp
- [X] T011 [US2] Extend busy indicator condition at RenderWindow.cpp:581 to include `isBoundingBoxComputationInProgress()` in gladius/src/ui/RenderWindow.cpp
- [ ] T012 [US2] Manual test: Verify busy indicator appears during graph edit → recompile cycle
- [ ] T013 [US2] Manual test: Verify busy indicator appears during parameter change → SDF update cycle
- [ ] T014 [US2] Manual test: Verify busy indicator disappears when computation completes

**Checkpoint**: Busy indicator now covers all compute states (compilation, SDF, bounding box)

---

## Phase 4: User Story 1 - Fluid Parameter Editing (Priority: P1)

**Goal**: Ensure UI remains responsive during parameter slider interactions

**Independent Test**: Drag any parameter slider rapidly while a model is displayed → verify no UI lag and preview updates progressively

**Note**: Core functionality already works via `tryToupdateParameter()`. This phase verifies and validates.

### Implementation for User Story 1

- [X] T015 [US1] Analyze `waitForComputeToken()` at RenderWindow.cpp:268 to determine if it blocks async preview path in gladius/src/ui/RenderWindow.cpp
- [X] T016 [US1] If blocking: Replace `waitForComputeToken()` with `requestComputeToken()` + skip frame pattern in gladius/src/ui/RenderWindow.cpp
- [ ] T017 [US1] Manual test: Verify parameter slider maintains 60fps responsiveness during drag
- [ ] T018 [US1] Manual test: Verify latest parameter value is used when GPU is busy with previous change

**Checkpoint**: Parameter editing confirmed non-blocking

---

## Phase 5: User Story 3 - Responsive Graph Editing (Priority: P2)

**Goal**: Ensure graph editor remains interactive during code generation and recompilation

**Independent Test**: Add/remove nodes while compilation is active → verify graph editing continues without lag

### Implementation for User Story 3

- [ ] T019 [US3] Verify `refreshModelAsync()` in Document.cpp runs code generation on background thread
- [ ] T020 [US3] Verify node add/delete operations don't wait for compute mutex
- [ ] T021 [US3] Manual test: Verify graph editor remains fluid during recompilation
- [ ] T022 [US3] Manual test: Verify navigation between functions is instant during compilation

**Checkpoint**: Graph editing confirmed non-blocking during compile

---

## Phase 6: User Story 4 - Responsive Bounding Box Updates (Priority: P3)

**Goal**: Ensure bounding box calculations happen asynchronously without UI freezes

**Independent Test**: Modify parameters that change model size → verify UI remains smooth while bounds are recalculated

### Implementation for User Story 4

- [ ] T023 [US4] Verify `updateBoundingBoxFast()` is only called from background thread (coroutine/worker)
- [ ] T024 [US4] Analyze `waitForComputeToken()` at RenderWindow.cpp:2387 in sdfPrecomputeCoroutine context
- [ ] T025 [US4] If blocking UI: Restructure coroutine to avoid main thread blocking in gladius/src/ui/RenderWindow.cpp
- [ ] T026 [US4] Manual test: Verify no UI freeze when modifying geometry-affecting parameters

**Checkpoint**: Bounding box updates confirmed non-blocking

---

## Phase 6.5: Coverage Verification (FR-003, FR-004, FR-007, FR-010)

**Purpose**: Verify requirements that are already implemented by existing infrastructure

- [ ] T031 [US3] Verify FR-003: Confirm `refreshModelAsync()` runs code generation on background thread (not UI) in gladius/src/Document.cpp
- [ ] T032 [US3] Verify FR-004: Confirm OpenCL compilation happens via background `std::async` in gladius/src/Document.cpp
- [ ] T033 [US1] Verify FR-007: Confirm `tryToupdateParameter()` implements last-write-wins coalescing in gladius/src/compute/ComputeCore.cpp
- [ ] T034 [US1] Verify FR-010: Confirm parameter changes within a frame preserve order (single-threaded UI ensures this) in gladius/src/ui/MainWindow.cpp

**Checkpoint**: All functional requirements have explicit verification

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and final validation

- [ ] T027 [P] Add Doxygen comments for new state query methods in gladius/src/compute/ComputeCore.h
- [ ] T028 [P] Update developer documentation if async patterns changed in docs/architecture/
- [ ] T029 Run full test suite to verify no regressions with "Run All Tests" task
- [ ] T030 Measure and document UI frame times during parameter editing (target: <16ms)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: N/A - no setup needed
- **Foundational (Phase 2)**: No dependencies - start immediately
- **User Story 2 (Phase 3)**: Depends on Phase 2 completion (needs state tracking)
- **User Story 1 (Phase 4)**: Depends on Phase 2 completion
- **User Story 3 (Phase 5)**: Depends on Phase 2 completion
- **User Story 4 (Phase 6)**: Depends on Phase 2 completion
- **Coverage Verification (Phase 6.5)**: Can run in parallel with user story phases
- **Polish (Phase 7)**: Depends on all user story phases complete

### User Story Dependencies

- **User Story 2 (P1 - Busy Indicator)**: FIRST priority - provides visual feedback for all other stories
- **User Story 1 (P1 - Parameter Editing)**: Can be validated after US2 (uses busy indicator)
- **User Story 3 (P2 - Graph Editing)**: Independent of US1, uses busy indicator from US2
- **User Story 4 (P3 - Bounding Box)**: Independent, uses busy indicator from US2

### Within Phase 2 (Foundational)

1. T001, T002 (atomic flags) - first
2. T003, T004 (query methods) [P] - after T001, T002
3. T005-T008 (set/clear flags) [P] - after T001, T002
4. T009 (unit test) - last

### Parallel Opportunities

```text
# After T001, T002 complete, these can run in parallel:
T003, T004, T005, T006, T007, T008

# After Phase 2 complete, all user stories can start in parallel:
Phase 3 (US2), Phase 4 (US1), Phase 5 (US3), Phase 6 (US4)

# Polish tasks can run in parallel:
T027, T028
```

---

## Parallel Example: Phase 2 (Foundational)

```bash
# First: Add atomic flags (sequential)
Task T001: "Add m_sdfComputationInProgress atomic flag"
Task T002: "Add m_boundingBoxComputationInProgress atomic flag"

# Then: All other Phase 2 tasks in parallel
Task T003: "Add isSdfComputationInProgress() query method"
Task T004: "Add isBoundingBoxComputationInProgress() query method"
Task T005: "Set m_sdfComputationInProgress = true at start"
Task T006: "Set m_sdfComputationInProgress = false at end"
Task T007: "Set m_boundingBoxComputationInProgress = true at start"
Task T008: "Set m_boundingBoxComputationInProgress = false at end"
```

---

## Implementation Strategy

### MVP First (User Story 2 - Busy Indicator)

1. Complete Phase 2: Foundational (T001-T009)
2. Complete Phase 3: User Story 2 (T010-T014)
3. **STOP and VALIDATE**: Busy indicator covers all compute states
4. Delivers immediate visual improvement - user sees busy indicator during all compute operations

### Incremental Delivery

1. Foundational → State tracking ready
2. Add User Story 2 → Busy indicator extended → **MVP COMPLETE**
3. Add User Story 1 → Parameter editing validated
4. Add User Story 3 → Graph editing validated
5. Add User Story 4 → Bounding box validated
6. Each story adds confidence without breaking previous work

---

## Notes

- [P] tasks = different files or logically independent, no dependencies
- [Story] label maps task to specific user story for traceability
- Most work is validation of existing async patterns
- Main implementation work is in Phase 2 (state tracking) and Phase 3 (busy indicator extension)
- Tasks T015, T016, T024, T025 may result in "no changes needed" if analysis shows existing code is already non-blocking
- Manual tests are critical - use parameter sliders and graph editing in the actual application
- Run "Build ALL (linux-releaseWithDebug)" task before manual testing

````
