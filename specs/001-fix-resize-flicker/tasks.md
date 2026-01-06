# Tasks: Fix Resize Flicker

**Feature**: Window Resize Without Flicker  
**Branch**: `001-fix-resize-flicker`  
**Input**: Design documents from `/specs/001-fix-resize-flicker/`

**Tests**: Manual visual testing per quickstart.md (no automated UI tests)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Ensure build environment and testing infrastructure is ready

- [X] T001 Verify build configuration in gladius/CMakePresets.json supports ReleaseWithDebug preset
- [X] T002 Create manual test documentation directory at gladius/tests/unittests/ui/
- [X] T003 [P] Copy quickstart.md test protocol to gladius/tests/unittests/ui/RenderWindowResizeTest.md

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core understanding and baseline establishment that ALL user stories depend on

**⚠️ CRITICAL**: No user story implementation can begin until this phase is complete

- [X] T004 Document current resize behavior by tracing code flow from RenderWindow::renderWindow() lines 488-503
- [X] T005 Document invalidateView() behavior and all side effects in gladius/src/ui/RenderWindow.cpp lines 612-623
- [X] T006 Identify all call sites of invalidateView() to understand impact of changes
- [X] T007 Verify async epoch cancellation mechanism in renderAsync() doesn't race with resize buffer reallocation
- [X] T008 Create baseline test recording of current flicker behavior (screen recording for reference)

**Checkpoint**: Foundation ready - detailed understanding of current resize handling established

---

## Phase 3: User Story 1 - Smooth Window Resize (Priority: P1) 🎯 MVP

**Goal**: Eliminate flicker during manual window resize operations (edge/corner dragging)

**Independent Test**: Follow Test 1.1-1.4 in quickstart.md - drag window edges/corners and verify no clearing/flicker

### Implementation for User Story 1

- [X] T009 [US1] Add m_preserveContentDuringResize and m_deferredResizePending flags to RenderWindow in gladius/src/ui/RenderWindow.h
- [X] T010 [US1] Modify resize detection logic in gladius/src/ui/RenderWindow.cpp lines 488-503 to set preserve flags instead of immediate invalidateView()
- [X] T011 [US1] Update invalidateView() in gladius/src/ui/RenderWindow.cpp lines 612-623 to conditionally skip framebuffer clearing when preserve flag is set
- [X] T012 [US1] Implement deferred buffer reallocation in renderAsync() - defer setScreenResolution() until current async render job completes (check epoch)
- [X] T013 [US1] Update renderSync() in gladius/src/ui/RenderWindow.cpp lines 1184-1361 to check preserve flag before clearing
- [X] T014 [US1] Update renderAsync() in gladius/src/ui/RenderWindow.cpp lines 1363-1643 to check preserve flag before clearing
- [X] T015 [US1] Add inline comments explaining framebuffer preservation and deferred reallocation logic
- [X] T016 [US1] Create gladius/tests/unittests/ui/RenderWindowResizeTest.cpp with GTest suite structure
- [X] T017 [US1] Write unit test: invalidateView() respects preserve flag and doesn't clear framebuffer
- [X] T018 [US1] Write unit test: Resize detection sets deferred reallocation flag correctly
- [X] T019 [US1] Write unit test: setScreenResolution() deferred until async epoch increments
- [X] T020 [US1] Build and run unit tests, verify all pass
- [ ] T021 [US1] Manual test: Execute Test 1.1 (single-edge horizontal resize) from quickstart.md
- [ ] T022 [US1] Manual test: Execute Test 1.2 (single-edge vertical resize) from quickstart.md
- [ ] T023 [US1] Manual test: Execute Test 1.3 (corner resize) from quickstart.md
- [ ] T024 [US1] Manual test: Execute Test 1.4 (rapid continuous resize stress test) from quickstart.md

**Checkpoint**: Manual window resize should now be flicker-free. Content remains visible during drag operations.

---

## Phase 4: User Story 2 - Maximize/Restore Stability (Priority: P2)

**Goal**: Ensure maximize/restore window operations preserve content without flicker

**Independent Test**: Follow Test 2.1-2.3 in quickstart.md - maximize and restore window, verify no clearing

### Implementation for User Story 2

- [ ] T025 [US2] Verify maximize/restore uses same resize detection code path as manual resize (no special handling needed)
- [ ] T026 [US2] Test that existing US1 changes handle maximize event correctly
- [ ] T027 [US2] Test that existing US1 changes handle restore event correctly
- [ ] T028 [US2] Manual test: Execute Test 2.1 (maximize transition) from quickstart.md
- [ ] T029 [US2] Manual test: Execute Test 2.2 (restore transition) from quickstart.md
- [ ] T030 [US2] Manual test: Execute Test 2.3 (rapid maximize/restore toggle) from quickstart.md

**Checkpoint**: Maximize and restore operations should preserve content without flicker, reusing US1 logic.

---

## Phase 5: User Story 3 - Multi-Monitor Drag Stability (Priority: P3)

**Goal**: Ensure window dragging between monitors with different DPI/resolution preserves content

**Independent Test**: Follow Test 3.1-3.2 in quickstart.md (requires multi-monitor setup) - drag between monitors

**Prerequisites**: Multi-monitor test environment (can be skipped if unavailable, documented as limitation)

### Implementation for User Story 3

- [ ] T031 [US3] Verify DPI changes are handled by existing ImGui::GetIO().FontGlobalScale logic (line 1120)
- [ ] T032 [US3] Verify multi-monitor transitions use same resize detection as single-monitor resize
- [ ] T033 [US3] Manual test: Execute Test 3.1 (standard-to-HiDPI transition) from quickstart.md if multi-monitor available
- [ ] T034 [US3] Manual test: Execute Test 3.2 (HiDPI-to-standard transition) from quickstart.md if multi-monitor available
- [ ] T035 [US3] Document multi-monitor behavior in code comments if tested, or note as untested if unavailable

**Checkpoint**: Multi-monitor transitions preserve content without flicker (or documented as untested).

---

## Phase 6: Edge Cases & Robustness

**Purpose**: Handle boundary conditions and ensure robustness across all scenarios

- [ ] T036 Manual test: Execute Test E.1 (resize to minimum dimensions) from quickstart.md
- [ ] T037 Manual test: Execute Test E.2 (resize during active rendering) from quickstart.md
- [ ] T038 Manual test: Execute Test E.3 (minimize and restore) from quickstart.md
- [ ] T039 Manual test: Execute Test E.4 (aspect ratio change widescreen↔portrait) from quickstart.md
- [ ] T040 Verify all edge case tests pass and document any known limitations
- [ ] T041 Add defensive checks: minimum viewport size validation (width/height >= 1px) before buffer allocation, null texture checks

**Checkpoint**: All edge cases handled gracefully without crashes or major visual artifacts.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Code quality, documentation, and final validation

- [ ] T042 [P] Code review: Verify Allman brace style, 4-space indentation, camelCase naming per constitution
- [ ] T043 [P] Code review: Verify modern C++ usage (no raw pointers, use const correctness, east-side const)
- [ ] T044 [P] Code review: Verify KISS principle - no unnecessary complexity added
- [ ] T045 Add final inline documentation for any non-obvious resize handling logic
- [ ] T046 Run full manual test suite from quickstart.md and verify all success criteria (SC-001 to SC-006)
- [ ] T047 Document any known limitations or edge cases in quickstart.md troubleshooting section
- [ ] T048 Update release notes with user-facing description of flicker fix
- [ ] T049 Create before/after screen recordings demonstrating the fix (optional, for documentation)

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup)
    ↓
Phase 2 (Foundational) ← BLOCKS ALL STORIES
    ↓
Phase 3 (US1: Manual Resize + Unit Tests) ← MVP GATE
    ↓
Phase 4 (US2: Maximize/Restore) ← Depends on US1 changes
    ↓
Phase 5 (US3: Multi-Monitor) ← Depends on US1 changes
    ↓
Phase 6 (Edge Cases) ← Depends on US1-US3 complete
    ↓
Phase 7 (Polish)
```

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - Core resize handling
- **User Story 2 (P2)**: Depends on User Story 1 completion - Reuses US1 resize logic for maximize/restore
- **User Story 3 (P3)**: Depends on User Story 1 completion - Reuses US1 resize logic for multi-monitor

**Note**: Unlike typical features, User Stories 2 and 3 are NOT independently implementable from US1 because they rely on the same underlying resize handling mechanism. However, each story CAN be independently tested to verify the fix works for that scenario.

### Critical Path

```
T001-T003 (Setup) → 
T004-T008 (Foundation) → 
T009-T024 (US1 Implementation + Unit Tests + Manual Tests) → 
T025-T030 (US2 Tests) → 
T031-T035 (US3 Tests) → 
T036-T041 (Edge Cases) → 
T042-T049 (Polish)
```

### Parallel Opportunities

**Phase 1 (Setup)**: All tasks T001-T003 can run in parallel (different files)

**Phase 2 (Foundational)**: Tasks can be partially parallel:
- T004 and T005 can run in parallel (reading different code sections)
- T006 depends on T004-T005 completion
- T007 can run in parallel with T004-T006

**Phase 3 (US1 Implementation)**:
- T008-T014 must be sequential (same files, dependent changes)
- T015-T018 (manual tests) can be batched and run together in one test session

**Phase 4 (US2 Testing)**:
- T019-T021 (verification) can run in parallel (reading code)
- T022-T024 (manual tests) can be batched and run together

**Phase 5 (US3 Testing)**:
- T025-T026 (verification) can run in parallel
- T027-T029 (manual tests) can be batched if multi-monitor setup available

**Phase 7 (Polish)**: 
- T036-T039 (code review items) can run in parallel
- T040-T043 depend on prior polish tasks

---

## Implementation Strategy

### MVP First (Minimum Viable Product)

1. **Complete Phase 1**: Setup (5 minutes)
2. **Complete Phase 2**: Foundational - understand current behavior (30 minutes)
3. **Complete Phase 3**: User Story 1 - manual resize fix (2-4 hours coding + testing)
4. **STOP and VALIDATE**: Execute all US1 manual tests (30 minutes)
5. **✅ MVP READY**: Core flicker issue is fixed for most common use case (manual resize)

At this point, you have a deployable improvement that solves 80% of the user pain.

### Incremental Delivery

After MVP (User Story 1):

6. **Add Phase 4**: User Story 2 - maximize/restore (1 hour verification + testing)
7. **Deploy/Demo**: Show improved maximize/restore behavior
8. **Add Phase 5**: User Story 3 - multi-monitor (1 hour verification + testing, if hardware available)
9. **Add Phase 6**: Edge cases robustness (2 hours)
10. **Add Phase 7**: Polish and finalize (2 hours)

**Total Estimated Time**: 
- MVP (US1 with unit tests): ~4-5 hours (3 hrs implementation + 1-2 hrs unit tests)
- Full feature (US1+US2+US3+polish): ~9-11 hours

### Single Developer Strategy

Work sequentially through phases:
1. Day 1 Morning: Setup + Foundational + US1 Implementation (MVP)
2. Day 1 Afternoon: US1 Testing + US2 Verification + US2 Testing
3. Day 2 Morning: US3 Testing (if multi-monitor available) + Edge Cases
4. Day 2 Afternoon: Polish + Final Validation

### Validation Checkpoints

After each phase, manually test the corresponding user story:
- **After US1 (T018)**: Manual resize must be flicker-free
- **After US2 (T024)**: Maximize/restore must be flicker-free
- **After US3 (T029)**: Multi-monitor drag must be flicker-free (or documented as untested)
- **After Edge Cases (T035)**: All edge case tests pass
- **After Polish (T043)**: All success criteria SC-001 to SC-006 verified

---

## Notes

- **No automated tests**: This is a visual UX fix; manual testing per quickstart.md is the validation method
- **File concentration**: All changes in 2 files (RenderWindow.h, RenderWindow.cpp) - no parallel file work possible
- **Testing time**: Manual test execution ~30-45 minutes per user story phase
- **Multi-monitor caveat**: US3 testing requires specific hardware; document if unavailable
- **Regression prevention**: Save screen recordings for future regression comparison
- **Constitution compliance**: Follow Modern C++, KISS, code style requirements throughout

---

## Success Validation

When all tasks are complete, verify against specification success criteria:

| Criteria | Verification Method | Expected Result |
|----------|-------------------|----------------|
| **SC-001** | Manual Test 1.1-1.4 | No clearing during resize in any direction |
| **SC-002** | All manual tests | 100% stable during all resize operations |
| **SC-003** | All manual tests | Zero clearing instances observed |
| **SC-004** | Visual observation | No blank frames or discontinuity |
| **SC-005** | Baseline recording comparison | No regressions vs. baseline behavior |
| **SC-006** | Code profiling (if needed) | Resize handling <1ms execution time |

All criteria must pass before considering the feature complete.
