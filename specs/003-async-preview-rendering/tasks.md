# Tasks: Asynchronous Preview Rendering

**Input**: Design documents from `/specs/003-async-preview-rendering/`  
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Add data structures and declarations needed by all user stories

- [X] T001 Add `PreviewRenderJob` and `PreviewResultMeta` structs in gladius/src/ui/render/AsyncRenderTypes.h
- [X] T002 [P] Add async preview state members to RenderWindow class in gladius/src/ui/RenderWindow.h
- [X] T003 [P] Add `renderLowResPreviewAsync()` declaration in gladius/src/compute/ComputeCore.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core async infrastructure that MUST complete before user stories

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Implement `renderLowResPreviewAsync()` returning `cl::Event` in gladius/src/compute/ComputeCore.cpp
- [X] T005 Add preview buffer pool (`PreviewBufferPool`) to AsyncRenderController in gladius/src/ui/render/AsyncRenderController.h
- [X] T006 [P] Implement `acquirePreviewBuffer()` method in gladius/src/ui/render/AsyncRenderController.cpp
- [X] T007 [P] Implement `publishPreviewFrame()` method in gladius/src/ui/render/AsyncRenderController.cpp
- [X] T008 Add `tryConsumePreviewResult()` method in gladius/src/ui/render/AsyncRenderController.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Fluid Camera Navigation (Priority: P1) 🎯 MVP

**Goal**: UI maintains 55+ FPS during continuous camera movement

**Independent Test**: Load any model, move camera continuously for 10 seconds, observe FPS counter stays above 55

### Implementation for User Story 1

- [X] T009 [US1] Implement `executeAsyncPreviewJob()` following `executeAsyncRenderJob()` pattern in gladius/src/ui/RenderWindow.cpp
- [X] T010 [US1] Implement `scheduleAsyncPreviewJob()` for non-blocking job enqueue in gladius/src/ui/RenderWindow.cpp
- [X] T011 [US1] Replace synchronous `renderLowResPreview()` call with `scheduleAsyncPreviewJob()` in `renderAsync()` path in gladius/src/ui/RenderWindow.cpp
- [X] T012 [US1] Remove `waitForComputeToken()` from preview rendering path in gladius/src/ui/RenderWindow.cpp
- [X] T013 [US1] Implement `processAsyncPreviewResults()` for non-blocking result polling in gladius/src/ui/RenderWindow.cpp
- [X] T014 [US1] Add result polling call in `renderWindow()` main loop in gladius/src/ui/RenderWindow.cpp

**Checkpoint**: User Story 1 complete - camera movement maintains 55+ FPS

---

## Phase 4: User Story 2 - Visual Feedback During Movement (Priority: P1)

**Goal**: Preview updates within 100ms of camera movement, HQ rendering starts 200ms after stopping

**Independent Test**: Move camera, verify preview updates reflect new viewpoint; stop and verify HQ rendering begins

### Implementation for User Story 2

- [ ] T015 [US2] Implement epoch-based job cancellation on camera input in gladius/src/ui/RenderWindow.cpp
- [ ] T016 [US2] Add frame ordering validation (no stale frames displayed after newer) in gladius/src/ui/RenderWindow.cpp
- [ ] T017 [US2] Implement `displayLastValidFrame()` fallback when no new result available in gladius/src/ui/RenderWindow.cpp
- [ ] T018 [US2] Add latency tracking in `PreviewResultMeta` consumption path in gladius/src/ui/RenderWindow.cpp
- [ ] T019 [US2] Ensure HQ progressive rendering resumes within 200ms after camera stops in gladius/src/ui/RenderWindow.cpp

**Checkpoint**: User Story 2 complete - visual feedback within 100ms, smooth HQ transition

---

## Phase 5: User Story 3 - Graceful Degradation Under Load (Priority: P2)

**Goal**: UI stays responsive even with complex models; preview frames may drop but no corruption

**Independent Test**: Load complex model, move camera rapidly, verify UI stays at 55+ FPS even if preview lags

### Implementation for User Story 3

- [ ] T020 [US3] Add buffer busy detection and skip logic in `scheduleAsyncPreviewJob()` in gladius/src/ui/RenderWindow.cpp
- [ ] T021 [US3] Implement preview frame dropping when worker can't keep up in gladius/src/ui/render/AsyncRenderController.cpp
- [ ] T022 [US3] Add error recovery for OpenCL failures in preview path in gladius/src/ui/RenderWindow.cpp
- [ ] T023 [US3] Add fallback to last valid frame on preview render failure in gladius/src/ui/RenderWindow.cpp
- [ ] T024 [US3] Log warning when preview rendering consistently exceeds latency target in gladius/src/ui/RenderWindow.cpp

**Checkpoint**: User Story 3 complete - graceful degradation under load

---

## Phase 6: Polish & Validation

**Purpose**: Testing, documentation, and cross-cutting improvements

- [ ] T025 [P] Add unit test for epoch-based cancellation in gladius/tests/unittests/AsyncPreviewRendering_tests.cpp
- [ ] T026 [P] Add unit test for buffer lifecycle (Idle→Writing→Ready→Front) in gladius/tests/unittests/AsyncPreviewRendering_tests.cpp
- [ ] T027 [P] Add integration test verifying 55+ FPS during camera movement in gladius/tests/unittests/AsyncPreviewRendering_tests.cpp
- [ ] T028 Add Doxygen comments for all new public APIs in gladius/src/ui/RenderWindow.h
- [ ] T029 [P] Add Doxygen comments for async preview APIs in gladius/src/compute/ComputeCore.h
- [ ] T030 Run quickstart.md validation checklist
- [ ] T031 Verify no memory leaks with ThreadSanitizer/AddressSanitizer; validate frame buffer memory usage does not exceed 2x baseline (SC-004)

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup)           → No dependencies
Phase 2 (Foundational)    → Depends on Phase 1; BLOCKS all user stories
Phase 3 (US1: FPS)        → Depends on Phase 2
Phase 4 (US2: Feedback)   → Depends on Phase 2; can parallel with US1 if careful
Phase 5 (US3: Graceful)   → Depends on Phase 2; can parallel with US1/US2
Phase 6 (Polish)          → Depends on all user stories
```

### User Story Independence

- **US1 (Fluid Navigation)**: Core async path - can complete and demo independently
- **US2 (Visual Feedback)**: Enhances US1 - requires US1 path but adds latency/ordering guarantees
- **US3 (Graceful Degradation)**: Resilience layer - independent error handling, testable separately

### Task Dependencies Within Phases

| Task | Depends On | Notes |
|------|------------|-------|
| T004 | T003 | Declaration before implementation |
| T006, T007 | T005 | Buffer pool before methods |
| T008 | T006, T007 | Result consumption after publish |
| T009 | T004, T008 | Executor needs async render + result queue |
| T010 | T009 | Scheduling uses executor |
| T011 | T010 | Integration uses scheduler |
| T013 | T008 | Uses `tryConsumePreviewResult()` |
| T014 | T013 | Main loop uses result processor |
| T015 | T011 | Cancellation after scheduling works |
| T025-T027 | T014 | Tests after feature complete |

---

## Parallel Opportunities

### Phase 1 Parallelization
```
T001 ─┬─ T002 (different files)
      └─ T003 (different files)
```

### Phase 2 Parallelization
```
T004 ──────────────────────────┐
T005 → T006 ─┬─ T007 (same file but different methods)
             └─ T008 (depends on both)
```

### User Story Parallelization
```
After Phase 2 completes:
├── US1 (T009-T014): Core async rendering
├── US2 (T015-T019): Can overlap with US1 tail
└── US3 (T020-T024): Independent error handling
```

### Phase 6 Parallelization
```
T025 ─┬─ T026 ─┬─ T027 (same test file, different tests)
      └─ T028 ─┴─ T029 (different header files)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003) - ~30 min
2. Complete Phase 2: Foundational (T004-T008) - ~2h
3. Complete Phase 3: User Story 1 (T009-T014) - ~3h
4. **STOP and VALIDATE**: Test 55+ FPS during camera movement
5. Demo/merge if ready

### Incremental Delivery

| Milestone | Tasks | Deliverable |
|-----------|-------|-------------|
| MVP | T001-T014 | 55+ FPS during camera movement |
| + Feedback | T015-T019 | <100ms preview latency, smooth HQ transition |
| + Resilience | T020-T024 | Graceful degradation under load |
| Complete | T025-T031 | Tests, docs, validation |

### Estimated Effort

| Phase | Tasks | Estimate |
|-------|-------|----------|
| Setup | T001-T003 | 30 min |
| Foundational | T004-T008 | 2h |
| US1 | T009-T014 | 3h |
| US2 | T015-T019 | 2h |
| US3 | T020-T024 | 2h |
| Polish | T025-T031 | 2h |
| **Total** | 31 tasks | ~11.5h |

---

## Notes

- All modifications follow existing patterns in `RenderWindow.cpp` and `AsyncRenderController`
- `RenderJobType::LowResPreview` already exists - reuse it
- Preview resolution uses existing `renderQualityWhileMoving` setting
- No new external dependencies required
- Tests require `GLADIUS_RUN_GPU_TESTS=1` environment variable
