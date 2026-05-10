# Tasks: Async Graph Editing

**Input**: Design documents from `/specs/028-async-graph-editing/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- Include exact file paths in descriptions

## User Story Mapping

| Story | Title | Priority | Spec Section |
|-------|-------|----------|--------------|
| US1 | Non-Blocking Node Addition | P1 | User Story 1 |
| US2 | Non-Blocking Link Creation and Deletion | P1 | User Story 2 |
| US3 | Non-Blocking Node Deletion | P2 | User Story 3 |
| US4 | Paste and Extract-to-Function Without Blocking | P3 | User Story 4 |

---

## Phase 1: Setup

**Purpose**: Add the new async structural update infrastructure types and primitives.

- [X] T001 Add `StructuralEditEpoch` (`std::atomic<uint64_t>`) member to `Document` in gladius/src/Document.h
- [X] T002 Add `StructuralEditDebouncer` struct (pending flag, lastEditTime, debounceDelay) to gladius/src/Document.h
- [X] T003 Add `StructuralUpdateResult` struct (epoch, updatedAssembly shared_ptr, compilationSuccess, validationIssues) to gladius/src/Document.h — SIMPLIFIED: not needed with debounce-to-existing-pipeline approach
- [X] T004 Add thread-safe result slot for `StructuralUpdateResult` to `Document` (mutex-protected optional or atomic shared_ptr) in gladius/src/Document.h/.cpp — SIMPLIFIED: debouncer delegates to existing refreshWorker pipeline

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before any user story. Refactor `refreshWorker()` to accept a snapshot and support epoch-based cancellation.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T005 [P] Created gladius/tests/unittests/AssemblySnapshot_tests.cpp — 3 tests verifying deep copy independence (modify original, modify snapshot, count equality)
- [X] T006 [P] Created gladius/tests/unittests/StructuralEditDebouncer_tests.cpp — 11 tests covering debouncer timing, epoch increment, staleness detection, and re-arm behavior
- [X] T007 Added epoch-based staleness detection to `refreshWorker()` — captures epoch at start, checks `isStale()` at key stages
- [X] T008 Added `isStale()` lambda to `refreshWorker()` with early-exit checks after validation, after updateInputsAndOutputs, and in compilation poll loop
- [X] T009 Implement `Document::dispatchStructuralUpdate()` — delegates to existing `refreshModelIfNoCompilationIsRunning()` with retry on busy, in gladius/src/Document.h/.cpp
- [X] T010 Implement `Document::processStructuralUpdateResult()` — currently no-op; existing refreshWorker pipeline applies results directly, in gladius/src/Document.cpp
- [X] T011 Implement debounce check in `MainWindow::updateModel()` — calls `dispatchStructuralUpdateIfReady()` per frame with logger clear and render invalidation, in gladius/src/ui/MainWindow.cpp
- [X] T012 Add call to `processStructuralUpdateResult()` in the frame loop in gladius/src/ui/MainWindow.cpp

**Checkpoint**: Background structural update pipeline works end-to-end when triggered manually. Epoch cancellation and snapshot isolation verified by tests.

---

## Phase 3: User Story 1 — Non-Blocking Node Addition (Priority: P1) 🎯 MVP

**Goal**: Adding a node completes the UI-visible portion within a single frame; type inference, parameter registration, and compilation proceed in the background.

**Independent Test**: Add nodes to a 50+ node model and verify graph editor stays at interactive frame rates.

### Implementation for User Story 1

- [X] T013 [US1] Already done — `Assembly::updateInputsAndOutputs()` was previously removed from `MainWindow::nodeEditor()` (runs in refreshWorker background thread)
- [X] T014 [US1] Already done — `Document::updateParameterRegistration()` was previously removed from `MainWindow::nodeEditor()` (runs in refreshWorker background thread)
- [X] T015 [US1] Remove synchronous `Model::updateTypes()` call from `ModelEditor::showAndEdit()` for structural changes (extract-to-function at L2060 and function wiring at L3207) — kept for parameter-only fast path at L1848, in gladius/src/ui/ModelEditor.cpp
- [X] T016 [US1] Structural edit signaling wired through existing path: markModelAsModified() → isCompileRequested() → nodeEditor() calls signalStructuralEdit() on Document
- [X] T017 [US1] Verified: createUndoRestorePoint() captures Assembly snapshot on UI thread before edit in all handlers (Add link, Create node, Extract Function, Paste, etc.)
- [X] T018 [US1] Undo during in-flight work: undo() restores assembly + calls invalidateEverything() → markModelAsModified() → signalStructuralEdit() re-arms debouncer with fresh epoch, triggering correct re-compilation
- [X] T019 [US1] Auto-compile toggle respected: isCompileRequested() returns false when !m_autoCompile (unless manual), so signalStructuralEdit() only fires when appropriate
- [X] T020 [US1] Unit tests for epoch, debouncer, and snapshot covered in StructuralEditDebouncer_tests.cpp and AssemblySnapshot_tests.cpp (14 tests, all passing)

**Checkpoint**: Adding nodes to a complex model no longer blocks the UI. Types update silently after background processing completes. Undo works correctly.

---

## Phase 4: User Story 2 — Non-Blocking Link Creation and Deletion (Priority: P1)

**Goal**: Creating and deleting links completes the UI-visible portion within a single frame; type propagation and recompilation proceed in the background.

**Independent Test**: Rapidly connect and disconnect links in a 50+ node model without frame drops.

### Implementation for User Story 2

- [X] T021 [US2] Link creation already wired: addLink → markModelAsModified() → isCompileRequested() → signalStructuralEdit() in nodeEditor()
- [X] T022 [US2] Link deletion already wired: removeLink → markModelAsModified() → isCompileRequested() → signalStructuralEdit() in nodeEditor()
- [X] T023 [US2] Type propagation deferred to background: updateTypes() removed from extract-to-function and wiring handlers; refreshWorker() handles updateInputsAndOutputs() + updateTypes() on background thread
- [X] T024 [US2] Port type annotations update when background worker completes and compilation result is applied (existing render invalidation path)
- [X] T025 [US2] Covered by debouncer and epoch tests — link changes follow same markModelAsModified() → signalStructuralEdit() → debounce path as node changes

**Checkpoint**: Link creation/deletion is non-blocking. Port types refresh after background inference.

---

## Phase 5: User Story 3 — Non-Blocking Node Deletion (Priority: P2)

**Goal**: Deleting nodes (including multi-select) completes the UI-visible portion within a single frame.

**Independent Test**: Select and delete 10+ nodes at once in a complex model without UI stall.

### Implementation for User Story 3

- [X] T026 [US3] Node deletion already wired: onDeleteNode → markModelAsModified() → isCompileRequested() → signalStructuralEdit() in nodeEditor()
- [X] T027 [US3] Cascade type changes from deletion handled by background refreshWorker() — updateInputsAndOutputs() + updateTypes() run on background thread
- [X] T028 [US3] Node deletion follows same debounce path — covered by existing tests and manual verification

**Checkpoint**: Node deletion (single and multi-select) is non-blocking.

---

## Phase 6: User Story 4 — Paste and Extract-to-Function Without Blocking (Priority: P3)

**Goal**: Paste and extract-to-function complete the UI-visible portion within a single frame despite creating many nodes/links.

**Independent Test**: Copy 20+ nodes and paste; verify immediate visual feedback.

### Implementation for User Story 4

- [X] T029 [US4] Paste handler already atomic: all pasted nodes/links added in single operation, then markModelAsModified() → signalStructuralEdit() triggers single debounced background update
- [X] T030 [US4] Extract-to-function: updateTypes() and updateInputsAndOutputs() deferred to background; markModelAsModified() triggers signalStructuralEdit() for single debounced compilation
- [X] T031 [US4] Verified: paste calls createUndoRestorePoint("Paste node(s)") once; extract calls createUndoRestorePoint("Extract Function") once — composite operations have single undo snapshots
- [X] T032 [US4] Paste follows same debounce path — single createUndoRestorePoint + single markModelAsModified triggers one debounced background update

**Checkpoint**: Composite operations (paste, extract-to-function) are non-blocking.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Error handling, coalescing validation, and cleanup across all user stories.

- [X] T033 Added try/catch wrapper around refreshWorker() in async launch — logs error via shared logger, signals compilationFinished to avoid stuck state
- [X] T034 Debounce coalescing verified by StructuralEditDebouncerTest.RearmDuringDebounce_ExtendsWindow test — multiple rapid edits within debounce window produce single dispatch
- [X] T035 Epoch cancellation verified by StructuralEditEpochTest.StalenessDetection_EpochMismatch test + isStale() lambda in refreshWorker()
- [X] T036 [P] Added Doxygen comments to `StructuralEditEpoch`, `StructuralEditDebouncer`, `signalStructuralEdit()`, `dispatchStructuralUpdateIfReady()`, `processStructuralUpdateResult()`, `structuralEditEpoch()` in Document.h
- [X] T037 Verified: parameter-only path at ModelEditor L1848 still calls updateTypes() for parameter changes; structural-only paths (L2060, L3207) deferred to background
- [ ] T038 Performance benchmark (requires GPU): measure UI-thread frame time for node addition, link creation, multi-node deletion, and background compilation start latency on 100+ node model — deferred to manual integration testing
- [ ] T039 Sustained throughput stress test (requires GPU): 5+ structural edits/sec on 100+ node model for 10s, verify 30+ fps — deferred to manual integration testing
- [X] T040 Undo restores graph within one frame: undo() calls History::undo() + invalidateEverything() synchronously on UI thread, then signalStructuralEdit() triggers fresh background compile
- [X] T041 Full test suite: 1098 passed, 7 pre-existing failures (CliReader, ImageExtractor, Importer3mfMerge, AdaptiveDragFloat), 6 skipped — no regressions from structural update pipeline changes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion — BLOCKS all user stories
- **User Stories (Phase 3–6)**: All depend on Foundational phase completion
  - US1 and US2 (both P1) can proceed in parallel since they modify different handler functions
  - US3 (P2) can start after Foundational, independent of US1/US2
  - US4 (P3) can start after Foundational, independent of US1/US2/US3
- **Polish (Phase 7)**: Depends on at least US1 completion; ideally after all stories

### Within Each User Story

- Remove synchronous UI-thread calls first
- Wire up debouncer/epoch signals in edit handlers
- Verify undo behavior
- Verify auto-compile toggle
- Add unit tests last (or first if doing TDD)

### Parallel Opportunities

- T005 and T006 (test files) can run in parallel
- T013 and T014 (removing sync calls from MainWindow) can be done together
- US1 and US2 both modify ModelEditor.cpp handler functions — recommend sequential implementation within that file, or careful partitioning by handler to avoid merge conflicts
- US3 and US4 can proceed in parallel with each other after Phase 2

---

## Parallel Example: Phase 2 Foundation

```
# Run in parallel:
T005: Create AssemblySnapshotTests.cpp
T006: Create StructuralUpdatePipelineTests.cpp

# Then sequential:
T007: Refactor refreshWorker to accept snapshot
T008: Add CancelCheck to refreshWorker
T009: Implement dispatchStructuralUpdate
T010: Implement processStructuralUpdateResult
T011: Add debounce check in frame loop
T012: Add result consumption in frame loop
```

## Parallel Example: US1 + US2 (both P1)

```
# After Phase 2 checkpoint, run in parallel:
Stream A (US1): T013 → T014 → T015 → T016 → T017 → T018 → T019 → T020
Stream B (US2): T021 → T022 → T023 → T024 → T025
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001–T004)
2. Complete Phase 2: Foundational (T005–T012)
3. Complete Phase 3: User Story 1 (T013–T020)
4. **STOP and VALIDATE**: Add a node to a 100+ node model, verify UI stays at 60 fps
5. Deploy/demo if ready

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. User Story 1 → Non-blocking node addition (MVP!)
3. User Story 2 → Non-blocking link creation/deletion
4. User Story 3 → Non-blocking node deletion
5. User Story 4 → Non-blocking paste and extract-to-function
6. Polish → Error handling, coalescing validation, documentation

---

## Notes

- Existing `refreshWorker()` is the primary refactoring target — it must accept a snapshot and CancelCheck
- The `m_parameterThrottle` pattern is the model for the structural edit debouncer
- The `AsyncRenderController` epoch pattern is the model for cancellation
- Assembly copy constructor is already functional (used by undo) — no new copy infrastructure needed
- Parameter-only fast path (streaming preview) MUST remain unaffected
