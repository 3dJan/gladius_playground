# Tasks: Export UI Lock

**Input**: Design documents from `/specs/008-export-ui-lock/`  
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Paths are relative to `gladius/` directory

---

## Phase 1: Setup

**Purpose**: Prepare codebase for export UI lock implementation

- [x] T001 Verify existing ExportState implementation in gladius/src/ui/ExportState.h
- [x] T002 [P] Document current components using ExportState (ModelEditor, ResourceView, BeamLatticeView, MainWindow)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Extend NodeView to access ExportState - required before any input blocking

**⚠️ CRITICAL**: User story work depends on NodeView having access to ExportState

- [x] T003 Add ExportState pointer member to NodeView in gladius/src/ui/NodeView.h
- [x] T004 Add setExportState() method to NodeView in gladius/src/ui/NodeView.h
- [x] T005 Wire NodeView::setExportState() from ModelEditor in gladius/src/ui/ModelEditor.cpp

**Checkpoint**: NodeView can now query export state

---

## Phase 3: User Story 1 - Model Protection During Export (Priority: P1) 🎯 MVP

**Goal**: Block all model-modifying interactions during export

**Independent Test**: Start export, attempt to modify parameters/nodes - all modifications blocked

### Implementation for User Story 1

- [x] T006 [US1] Add export state check to ModelEditor keyboard shortcuts (Ctrl+C/V, Delete) in gladius/src/ui/ModelEditor.cpp
- [x] T007 [US1] Add export state check to ModelEditor Undo/Redo menu items in gladius/src/ui/ModelEditor.cpp
- [x] T008 [US1] Add BeginDisabled/EndDisabled wrapper around viewFloat() in gladius/src/ui/NodeView.cpp
- [x] T009 [P] [US1] Add BeginDisabled/EndDisabled wrapper around viewFloat3() in gladius/src/ui/NodeView.cpp
- [x] T010 [P] [US1] Add BeginDisabled/EndDisabled wrapper around viewString() in gladius/src/ui/NodeView.cpp
- [x] T011 [P] [US1] Add BeginDisabled/EndDisabled wrapper around viewInt() in gladius/src/ui/NodeView.cpp
- [x] T012 [P] [US1] Add BeginDisabled/EndDisabled wrapper around viewMatrix() in gladius/src/ui/NodeView.cpp
- [x] T013 [P] [US1] Add BeginDisabled/EndDisabled wrapper around viewResource() in gladius/src/ui/NodeView.cpp
- [x] T014 [US1] Verify onCreateNode() already blocks during export in gladius/src/ui/ModelEditor.cpp
- [x] T015 [US1] Verify onDeleteNode() already blocks during export in gladius/src/ui/ModelEditor.cpp

**Checkpoint**: All model-modifying inputs are blocked during export

---

## Phase 4: User Story 2 - Visual Feedback During Export Lock (Priority: P1)

**Goal**: Display semi-transparent overlay with status message during export

**Independent Test**: Start export, observe dark overlay with "Export in progress..." message

### Implementation for User Story 2

- [x] T016 [US2] Add renderExportOverlay() private method declaration to gladius/src/ui/ModelEditor.h
- [x] T017 [US2] Implement renderExportOverlay() using GetWindowDrawList in gladius/src/ui/ModelEditor.cpp
- [x] T018 [US2] Call renderExportOverlay() after ed::End() in showAndEdit() in gladius/src/ui/ModelEditor.cpp
- [x] T019 [US2] Add centered text rendering with "Export in progress..." message in gladius/src/ui/ModelEditor.cpp

**Checkpoint**: Overlay appears during export and disappears on completion

---

## Phase 5: User Story 3 - Block File Operations During Export (Priority: P1)

**Goal**: Prevent file operations (New, Open, Import) during export

**Independent Test**: Start export, verify File menu items are disabled

### Implementation for User Story 3

- [x] T020 [US3] Verify File > New is disabled during export in gladius/src/ui/MainWindow.cpp
- [x] T021 [US3] Verify File > Open is disabled during export in gladius/src/ui/MainWindow.cpp
- [x] T022 [US3] Verify Import functions is disabled during export in gladius/src/ui/MainWindow.cpp
- [x] T023 [US3] Add close confirmation when export is in progress in gladius/src/ui/MainWindow.cpp (if not present)

**Checkpoint**: File operations blocked, close confirmation works

---

## Phase 6: User Story 4 - Format-Agnostic Protection (Priority: P2)

**Goal**: Verify UI lock works for all export formats and mesh extraction methods

**Independent Test**: Test STL/3MF exports with MDC/HDC - overlay should appear in all cases

### Implementation for User Story 4

- [ ] T024 [US4] Manual test: STL export with MDC shows overlay
- [ ] T025 [US4] Manual test: 3MF export with MDC shows overlay
- [ ] T026 [US4] Manual test: Export via MCP API shows overlay (if applicable)

**Checkpoint**: All export types trigger UI lock

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, testing, cleanup

- [x] T027 [P] Add Doxygen comments to new public methods in NodeView.h
- [x] T028 [P] Add Doxygen comments to renderExportOverlay() in ModelEditor.h
- [x] T029 Create ExportState unit tests in gladius/tests/unittests/ExportState_tests.cpp
- [ ] T030 Run quickstart.md verification steps (requires manual testing)
- [x] T031 Update developer_onboarding.md if needed (reviewed - no changes needed)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - verification only
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational phase (NodeView needs ExportState access)
- **User Story 2 (Phase 4)**: Can start after Foundational (independent of US1)
- **User Story 3 (Phase 5)**: Can start after Setup (uses existing ExportState in MainWindow)
- **User Story 4 (Phase 6)**: Depends on US1 + US2 completion (verification)
- **Polish (Phase 7)**: Depends on all user stories

### Parallel Opportunities Within User Story 1

```text
# After T008 (viewFloat), these can run in parallel:
T009 [P] viewFloat3()
T010 [P] viewString()
T011 [P] viewInt()
T012 [P] viewMatrix()
T013 [P] viewResource()
```

### Parallel Opportunities Across Stories

```text
# After Foundational phase (T005), these user stories can run in parallel:
- User Story 1 (T006-T015)
- User Story 2 (T016-T019)
- User Story 3 (T020-T023) - can actually start earlier since it uses MainWindow's ExportState
```

---

## Implementation Strategy

### MVP First (User Story 1 + 2 Only)

1. Complete Phase 1: Setup (T001-T002)
2. Complete Phase 2: Foundational (T003-T005)
3. Complete Phase 3: User Story 1 - Input Blocking (T006-T015)
4. Complete Phase 4: User Story 2 - Visual Overlay (T016-T019)
5. **STOP and VALIDATE**: Test with manual export

### Full Implementation

1. Complete MVP phases above
2. Complete Phase 5: User Story 3 - File Operations (T020-T023)
3. Complete Phase 6: User Story 4 - Format Verification (T024-T026)
4. Complete Phase 7: Polish (T027-T031)

---

## Notes

- [P] tasks = different files/methods, no dependencies
- [US#] label maps task to specific user story
- T014, T015, T020-T022 are verification tasks (code already exists)
- T024-T026 are manual testing tasks
- File operations (US3) are already partially implemented - mainly verification
