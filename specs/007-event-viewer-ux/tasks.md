# Tasks: Event Viewer UX Improvements

**Input**: Design documents from `/specs/007-event-viewer-ux/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, quickstart.md ✅

**Tests**: Unit tests included for testable formatting logic. UI interactions tested manually.

**Organization**: Tasks grouped by user story to enable independent implementation.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `gladius/src/ui/LogView.h`, `gladius/src/ui/LogView.cpp`
- **Tests**: `gladius/tests/unittests/ui/LogView_tests.cpp`

---

## Phase 1: Setup

**Purpose**: Create test infrastructure for new functionality

- [ ] T001 Create test file structure at gladius/tests/unittests/ui/LogView_tests.cpp
- [ ] T002 Add LogView_tests.cpp to CMakeLists.txt in gladius/tests/unittests/CMakeLists.txt

---

## Phase 2: Foundational (Shared Helper Functions)

**Purpose**: Core infrastructure used by multiple user stories

- [ ] T003 Add formatEventForClipboard() helper function in gladius/src/ui/LogView.cpp
- [ ] T004 Add formatEventsForClipboard() helper for multiple events in gladius/src/ui/LogView.cpp
- [ ] T005 [P] Add unit tests for formatEventForClipboard() in gladius/tests/unittests/ui/LogView_tests.cpp

**Checkpoint**: Helper functions ready - user story implementation can begin

---

## Phase 3: User Story 1 - Default Severity Filter (Priority: P1) 🎯 MVP

**Goal**: Event Viewer shows only warnings, errors, and fatal errors by default (hides Info)

**Independent Test**: Open application, generate Info/Warning/Error events, open Event Viewer → only Warning/Error/Fatal visible

### Implementation for User Story 1

- [ ] T006 [US1] Change m_showInfo default from true to false in gladius/src/ui/LogView.h
- [ ] T007 [US1] Update updateCache() call on first render to respect new default in gladius/src/ui/LogView.cpp

**Checkpoint**: User Story 1 complete - default filter now hides Info messages

---

## Phase 4: User Story 2 - Copy Individual Event (Priority: P2)

**Goal**: Users can right-click an event to copy it, or use Ctrl+C

**Independent Test**: Right-click any event → select "Copy" → paste in text editor → verify formatted output

### Implementation for User Story 2

- [ ] T008 [US2] Add m_selectedEventIndex member variable to LogView in gladius/src/ui/LogView.h
- [ ] T009 [US2] Wrap event rows in ImGui::PushID/PopID for unique context menu IDs in gladius/src/ui/LogView.cpp
- [ ] T010 [US2] Add ImGui::BeginPopupContextItem() for right-click menu in renderExpandedView() in gladius/src/ui/LogView.cpp
- [ ] T011 [US2] Implement "Copy" menu item that calls ImGui::SetClipboardText() in gladius/src/ui/LogView.cpp
- [ ] T012 [US2] Add Ctrl+C keyboard shortcut handling in renderExpandedView() in gladius/src/ui/LogView.cpp

**Checkpoint**: User Story 2 complete - individual event copy works via right-click and Ctrl+C

---

## Phase 5: User Story 3 - Copy All Visible Events (Priority: P2)

**Goal**: Users can click "Copy All" button to copy all filtered/visible events

**Independent Test**: Apply filter → click "Copy All" → paste in text editor → verify only filtered events appear

### Implementation for User Story 3

- [ ] T013 [US3] Add "Copy All" button to toolbar in render() after "Clear log" button in gladius/src/ui/LogView.cpp
- [ ] T014 [US3] Implement copyAllVisibleEvents() that iterates filtered events and formats for clipboard in gladius/src/ui/LogView.cpp

**Checkpoint**: User Story 3 complete - bulk copy respects all active filters

---

## Phase 6: User Story 4 - Copy by Severity from Collapsed View (Priority: P3)

**Goal**: Users can copy all events of a specific severity from collapsed view popups

**Independent Test**: In collapsed view → click error count → click "Copy All Errors" → verify all errors copied

### Implementation for User Story 4

- [ ] T015 [US4] Add copyEventsBySeverity(Severity) helper function in gladius/src/ui/LogView.cpp
- [ ] T016 [US4] Convert fatal error tooltip to popup with copy button in renderCollapsedView() in gladius/src/ui/LogView.cpp
- [ ] T017 [US4] Convert error tooltip to popup with copy button in renderCollapsedView() in gladius/src/ui/LogView.cpp
- [ ] T018 [US4] Convert warning tooltip to popup with copy button in renderCollapsedView() in gladius/src/ui/LogView.cpp

**Checkpoint**: User Story 4 complete - per-severity copy available in collapsed view

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, cleanup, and validation

- [ ] T019 [P] Add Doxygen comments for new public/private methods in gladius/src/ui/LogView.h
- [ ] T020 [P] Add copy feedback tooltip ("Copied!") for visual confirmation in gladius/src/ui/LogView.cpp
- [ ] T021 Run quickstart.md manual testing checklist
- [ ] T022 Build and run all unit tests to verify no regressions

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion
- **User Story 1 (Phase 3)**: No dependency on Phase 2 (separate functionality)
- **User Stories 2-4 (Phases 4-6)**: Depend on Phase 2 (use helper functions)
- **Polish (Phase 7)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Independent - can complete and deploy as MVP
- **User Story 2 (P2)**: Requires Phase 2 helpers
- **User Story 3 (P2)**: Requires Phase 2 helpers, can run parallel with US2
- **User Story 4 (P3)**: Requires Phase 2 helpers, can run parallel with US2/US3

### Parallel Opportunities

```bash
# Phase 2 parallel tasks:
T003 + T005  # Helper function + its tests (different files)

# User Stories 2, 3, 4 can run in parallel after Phase 2 completes
# (all modify LogView.cpp but touch different functions)

# Phase 7 parallel tasks:
T019 + T020  # Documentation + feedback (different concerns)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete T001-T002 (Setup)
2. Complete T006-T007 (User Story 1)
3. **STOP and VALIDATE**: Open app, verify Info hidden by default
4. Deploy if ready - delivers immediate value

### Incremental Delivery

1. Phase 1 + US1 → Default filter working (MVP)
2. Phase 2 + US2 → Right-click copy working
3. US3 → Copy All button working
4. US4 → Collapsed view copy working
5. Phase 7 → Polish and documentation

### Estimated Time Per Phase

| Phase | Tasks | Estimated Time |
|-------|-------|----------------|
| Setup | T001-T002 | 10 min |
| Foundational | T003-T005 | 20 min |
| User Story 1 | T006-T007 | 5 min |
| User Story 2 | T008-T012 | 30 min |
| User Story 3 | T013-T014 | 15 min |
| User Story 4 | T015-T018 | 30 min |
| Polish | T019-T022 | 20 min |
| **Total** | 22 tasks | **~2 hours** |

---

## Notes

- [P] tasks = different files or independent concerns
- [Story] label maps task to user story for traceability
- Each user story can be tested independently after completion
- Build after each phase to catch errors early
- Commit after each completed user story
