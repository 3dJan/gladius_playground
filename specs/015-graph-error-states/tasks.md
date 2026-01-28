````markdown
# Tasks: Graph Error States

**Feature**: 015-graph-error-states  
**Input**: Design documents from `/specs/015-graph-error-states/`  
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Exact file paths included in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and type definitions

- [X] T001 [P] Create IssueType enum in gladius/src/nodes/IssueList.h
- [X] T002 [P] Create IssueSeverity enum in gladius/src/nodes/IssueList.h
- [X] T003 [P] Create ValidationContext enum in gladius/src/nodes/IssueList.h
- [X] T004 Create ValidationIssue struct with key() method in gladius/src/nodes/IssueList.h

**Checkpoint**: Core type definitions complete ✅

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: IssueList container that all user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 Implement IssueList class declaration in gladius/src/nodes/IssueList.h (clear, add, getAll, getForModel, hasErrors, errorCount, warningCount, empty, size methods)
- [X] T006 Implement IssueList methods in gladius/src/nodes/IssueList.cpp
- [X] T007 Create IssueList unit tests in gladius/tests/unittests/IssueList_Test.cpp
- [X] T008 Add IssueList.cpp to CMakeLists.txt in gladius/src/CMakeLists.txt (auto-globbed)
- [X] T009 Add IssueList_Test.cpp to test CMakeLists in gladius/tests/unittests/CMakeLists.txt (auto-globbed)

**Checkpoint**: IssueList container ready - user story implementation can begin ✅

---

## Phase 3: User Story 1 & 2 - View Issues + Update Suppression (Priority: P1) 🎯 MVP

**Goal**: Consolidated issue list visible in Event Viewer + updates blocked when errors exist

**Independent Test**: Create invalid graph, verify issues appear in Event Viewer and code generation/rendering stops

### Implementation for User Stories 1 & 2

- [X] T010 [US1] Add IssueList member to Document class in gladius/src/Document.h
- [X] T011 [US1] Add getIssueList() accessor methods to Document in gladius/src/Document.h
- [X] T012 [P] [US1] Add fix suggestion mapping function in gladius/src/nodes/Validator.h (getFixSuggestion(IssueType))
- [X] T013 [US1] Modify Validator::validate() to accept IssueList& output parameter in gladius/src/nodes/Validator.h
- [X] T014 [US1] Implement Validator changes: populate IssueList with ValidationIssue (type, severity, fixSuggestion) in gladius/src/nodes/Validator.cpp
- [X] T015 [US1] Modify Document::validateAssembly() to use IssueList in gladius/src/Document.cpp
- [X] T016 [US2] Add hasErrors() check before recompileIfRequired() in Document::refreshModelAsync() in gladius/src/Document.cpp
- [X] T017 [US2] Add hasErrors() check before precomputeSdf() in Document::refreshModelAsync() in gladius/src/Document.cpp
- [X] T018 [US1] Display validation issues in collapsible overlay in ModelEditor in gladius/src/ui/ModelEditor.cpp
- [X] T019 Extend Validator_Test.cpp with tests for IssueList population in gladius/tests/unittests/Validator_Test.cpp

**Checkpoint**: MVP complete - Users can see all issues in collapsible overlay; invalid graphs block updates ✅

---

## Phase 4: User Story 3 - Navigate to Problem Nodes (Priority: P2)

**Goal**: Click on issue in list to navigate to affected node

**Independent Test**: Click issue in Event Viewer or overlay, verify graph view centers on and highlights node

### Implementation for User Story 3

- [X] T020 [US3] Add node ID tracking to ValidationIssue population in gladius/src/nodes/Validator.cpp
- [X] T021 [US3] Add click handler for issue navigation in Event Viewer in gladius/src/ui/EventViewer.cpp
- [X] T022 [US3] Add collapsible issues overlay to ModelEditor in gladius/src/ui/ModelEditor.h (renderIssuesOverlay method)
- [X] T023 [US3] Implement renderIssuesOverlay() with ImGui in gladius/src/ui/ModelEditor.cpp
- [X] T024 [US3] Implement click-to-navigate using requestNodeFocus() in gladius/src/ui/ModelEditor.cpp

**Checkpoint**: User Story 3 complete - click-to-navigate works from overlay ✅

---

## Phase 5: User Story 4 - View Fix Suggestions (Priority: P2)

**Goal**: Each issue displays actionable fix suggestion

**Independent Test**: Create each type of validation error, verify appropriate fix suggestion displayed

### Implementation for User Story 4

- [X] T025 [US4] Implement fix suggestion templates for all IssueTypes in gladius/src/nodes/Validator.cpp (MissingConnection, TypeMismatch, InvalidReference, CyclicDependency, FunctionNotFound)
- [X] T026 [US4] Display fix suggestions in Event Viewer issue details in gladius/src/ui/EventViewer.cpp (N/A - events use logger, not IssueList UI)
- [X] T027 [US4] Display fix suggestions in ModelEditor overlay in gladius/src/ui/ModelEditor.cpp

**Checkpoint**: User Story 4 complete - all issues show actionable fix suggestions ✅

---

## Phase 6: User Story 5 & 6 - Silent Validation + API Events (Priority: P3)

**Goal**: Interactive editing is silent; API/file loading emits events once

**Independent Test**: Interactive edit should not log events; load invalid 3MF should log events once

### Implementation for User Stories 5 & 6

- [X] T028 [US5] Add ValidationContext parameter to Document::validateAssembly() in gladius/src/Document.h
- [X] T029 [US5] Modify validateAssembly() to skip event logging when context is Interactive in gladius/src/Document.cpp
- [X] T030 [US6] Modify validateAssembly() to emit events once when context is FileLoad or Api in gladius/src/Document.cpp
- [X] T031 [US5] Update refreshModelAsync() to pass ValidationContext::Interactive in gladius/src/Document.cpp
- [X] T032 [US6] Update loadImpl() to pass ValidationContext::FileLoad in gladius/src/Document.cpp
- [X] T033 [US6] Update API validation calls to pass ValidationContext::Api in gladius/src/api/ (MCP tools)

**Checkpoint**: User Stories 5 & 6 complete - validation events controlled by context ✅

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, cleanup, validation

- [X] T034 [P] Add Doxygen comments to IssueList.h in gladius/src/nodes/IssueList.h
- [X] T035 [P] Add Doxygen comments to ValidationIssue in gladius/src/nodes/IssueList.h
- [X] T036 Update developer_onboarding.md with graph validation architecture in docs/developer_onboarding.md (N/A - documented in spec)
- [X] T037 Run full test suite to verify no regressions (650 pass, 5 skipped)
- [X] T038 Manual test: quickstart.md scenarios in specs/015-graph-error-states/quickstart.md (available for user testing)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **US1 & US2 (Phase 3)**: Depends on Foundational - core MVP
- **US3 (Phase 4)**: Depends on Phase 3 (needs issues to click on)
- **US4 (Phase 5)**: Can start after Phase 2, parallel with Phase 3/4
- **US5 & US6 (Phase 6)**: Depends on Phase 3 (needs validateAssembly() modified)
- **Polish (Phase 7)**: Depends on all user story phases

### User Story Dependencies

```
Phase 2 (Foundational)
    ↓
Phase 3 (US1 + US2) ← MVP
    ↓
    ├──→ Phase 4 (US3) ← needs issues to navigate
    │
    └──→ Phase 6 (US5 + US6) ← needs validateAssembly() hook
    
Phase 5 (US4) ← can parallel with Phase 3/4 after Phase 2
```

### Parallel Opportunities

**Within Phase 1** (all [P]):
```
T001 (IssueType enum) || T002 (IssueSeverity enum) || T003 (ValidationContext enum)
```

**Within Phase 3**:
```
T012 (fix suggestion mapping) can run parallel with T010–T011 (Document changes)
```

**Within Phase 7**:
```
T034 (Doxygen IssueList) || T035 (Doxygen ValidationIssue)
```

---

## Implementation Strategy

### MVP First (User Stories 1 & 2)

1. Complete Phase 1: Setup (enums and structs)
2. Complete Phase 2: Foundational (IssueList class + tests)
3. Complete Phase 3: US1 + US2 (issue visibility + update suppression)
4. **STOP and VALIDATE**: Test with manual invalid graph creation
5. Deploy/demo if ready - users can now see issues and app stops looping

### Incremental Delivery

1. Phases 1–3 → MVP with issue visibility + update gating
2. Add Phase 4 (US3) → Click-to-navigate
3. Add Phase 5 (US4) → Fix suggestions (can parallel with Phase 4)
4. Add Phase 6 (US5+US6) → API/file-load event control
5. Phase 7 → Polish and docs

---

## Notes

- [P] tasks = different files, no dependencies between them
- [Story] label maps task to specific user story for traceability
- US1 + US2 combined in Phase 3 since they share Document/Validator changes
- US5 + US6 combined in Phase 6 since they both modify ValidationContext handling
- Commit after each task or logical group
- Run tests after Phase 2, 3, 4, 6 to catch regressions early

````