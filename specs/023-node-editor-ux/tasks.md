# Tasks: Node Editor UX Improvements

**Input**: Design documents from `/specs/023-node-editor-ux/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `quickstart.md`, `contracts/widget-api.md`, `contracts/node-editor-interactions.md`

**Tests**: Included. The plan and constitution require unit-test coverage for core interaction logic, plus manual validation via `quickstart.md`.

**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently.

## Phase 1: Setup

**Purpose**: Prepare the feature workspace, test scaffolding, and module boundaries used by all stories.

- [X] T001 Review and refresh source/test file touchpoints in `gladius/src/ui/NodeView.cpp`, `gladius/src/ui/ModelEditor.cpp`, and `gladius/tests/unittests/CMakeLists.txt`
- [X] T002 [P] Create or refresh numeric widget declarations in `gladius/src/ui/NumericWidgets.h`
- [X] T003 [P] Create or refresh numeric widget implementation file in `gladius/src/ui/NumericWidgets.cpp`
- [X] T004 [P] Create or refresh responsiveness throttle declarations in `gladius/src/ui/ParameterThrottle.h`
- [X] T005 [P] Create or refresh responsiveness throttle implementation in `gladius/src/ui/ParameterThrottle.cpp`
- [X] T006 [P] Create or refresh link-drag state declarations in `gladius/src/ui/LinkDragState.h`
- [X] T007 [P] Create or refresh link-drag state implementation in `gladius/src/ui/LinkDragState.cpp`
- [X] T008 Register any new or updated UI and test files in `gladius/src/ui/CMakeLists.txt` and `gladius/tests/unittests/CMakeLists.txt`

---

## Phase 2: Foundational

**Purpose**: Establish shared styling, shared pin interaction rules, and helper seams that all user stories depend on.

**⚠️ CRITICAL**: No user story work should begin until this phase is complete.

- [X] T009 Define shared node style metrics and helper APIs in `gladius/src/ui/Style.h`
- [X] T010 Implement shared node style push/pop helpers and hashed category-color fallback in `gladius/src/ui/Style.cpp`
- [X] T011 [P] Define shared pin visual state tokens in `gladius/src/ui/LinkColors.h`
- [X] T012 [P] Add shared pin interaction declarations and comments in `gladius/src/ui/NodeView.h`
- [X] T013 Implement shared pin rendering baseline in `gladius/src/ui/NodeView.cpp`
- [X] T014 Add foundational regression coverage for shared pin behavior in `gladius/tests/unittests/NodeView_tests.cpp`

**Checkpoint**: Shared interaction and style infrastructure is ready; user stories can now proceed independently.

---

## Phase 3: User Story 1 - Intuitive Numeric Value Editing (Priority: P1) 🎯 MVP

**Goal**: Deliver inline dial + drag-float numeric editing with adaptive sensitivity, keyboard support, direct entry, and optional slider presentation.

**Independent Test**: Open a model with numeric parameters and verify drag, Shift/Ctrl modifiers, arrow-key editing, direct text entry, and dial interaction all update values immediately and proportionally.

### Tests for User Story 1

- [X] T015 [P] [US1] Add adaptive sensitivity unit tests in `gladius/tests/unittests/NumericWidget_tests.cpp`
- [X] T016 [P] [US1] Add bounded/unbounded dial behavior tests in `gladius/tests/unittests/NumericWidget_tests.cpp`
- [X] T017 [P] [US1] Add layout-mode and synchronization tests in `gladius/tests/unittests/NumericWidget_tests.cpp`

### Implementation for User Story 1

- [X] T018 [US1] Implement adaptive drag-float behavior in `gladius/src/ui/NumericWidgets.cpp`
- [X] T019 [US1] Implement orbital dial rendering and interaction in `gladius/src/ui/NumericWidgets.cpp`
- [X] T020 [US1] Implement composite dial-plus-drag-float and slider layout selection in `gladius/src/ui/NumericWidgets.cpp`
- [X] T021 [US1] Integrate shared numeric widgets into scalar parameter rendering in `gladius/src/ui/NodeView.cpp`
- [X] T022 [US1] Integrate shared numeric widgets into vector parameter rendering in `gladius/src/ui/NodeView.cpp`
- [X] T023 [US1] Persist per-parameter layout mode metadata in `gladius/src/nodes/Model.h` and `gladius/src/nodes/Model.cpp`
- [ ] T024 [US1] Validate numeric widget flows from `specs/023-node-editor-ux/quickstart.md` in `gladius/tests/unittests/NumericWidget_tests.cpp`

**Checkpoint**: User Story 1 is independently functional and testable.

---

## Phase 4: User Story 2 - Visual Port Compatibility Highlighting During Linking (Priority: P1)

**Goal**: Highlight compatible ports, dim incompatible ones, and keep drag/drop behavior reliable across all node types.

**Independent Test**: Start a link drag from inputs and outputs in a graph containing mixed node types; confirm compatible ports highlight immediately, incompatible ports dim, tooltips appear, and invalid drops are rejected.

### Tests for User Story 2

- [X] T025 [P] [US2] Add drag-session state and reset tests in `gladius/tests/unittests/LinkDragState_tests.cpp`
- [X] T026 [P] [US2] Add compatibility resolution tests for static and dynamic types in `gladius/tests/unittests/LinkDragState_tests.cpp`
- [X] T027 [P] [US2] Add node-view pin highlighting regression tests in `gladius/tests/unittests/NodeView_tests.cpp`

### Implementation for User Story 2

- [X] T028 [US2] Implement drag-session compatibility computation in `gladius/src/ui/LinkDragState.cpp`
- [X] T029 [US2] Expose model-driven compatibility helpers in `gladius/src/nodes/Model.h` and `gladius/src/nodes/Model.cpp`
- [X] T030 [US2] Integrate drag-session lifecycle into `gladius/src/ui/ModelEditor.cpp`
- [X] T031 [US2] Apply highlight/dim and tooltip behavior to shared pin rendering in `gladius/src/ui/NodeView.cpp`
- [X] T032 [US2] Ensure compact and regular nodes both use the same drag-start and hit-target path in `gladius/src/ui/NodeView.cpp`
- [X] T033 [US2] Validate invalid-drop rejection and Escape-cancel cleanup in `gladius/src/ui/ModelEditor.cpp`

**Checkpoint**: User Story 2 is independently functional and testable.

---

## Phase 5: User Story 3 - Compact, Stylish Node Rendering Without Clipping (Priority: P2)

**Goal**: Deliver compact rounded/capsule nodes with unclipped content, consistent pin alignment, and visually cohesive styling.

**Independent Test**: Render simple and complex nodes side by side at default and reduced zoom; confirm no clipping, consistent port sizes, readable labels, and aligned link anchors.

### Tests for User Story 3

- [X] T034 [P] [US3] Add compact-node layout regression tests in `gladius/tests/unittests/NodeView_tests.cpp`
- [X] T035 [P] [US3] Add node layout stability coverage for compact and fallback layouts in `gladius/tests/integrationtests/NodeLayoutEngine_tests.cpp`

### Implementation for User Story 3

- [X] T036 [US3] Rework compact node layout around shared left/right pin rails in `gladius/src/ui/NodeView.cpp`
- [X] T037 [US3] Implement compact-node fallback expansion rules in `gladius/src/ui/NodeView.cpp`
- [X] T038 [US3] Apply rounded or capsule styling consistently across compact node variants in `gladius/src/ui/NodeView.cpp` and `gladius/src/ui/Style.cpp`
- [X] T039 [US3] Implement display-name truncation and hover reveal in `gladius/src/ui/NodeView.cpp`
- [X] T040 [US3] Replace remaining fixed-width node sizing with content-measured sizing in `gladius/src/ui/NodeView.cpp`
- [X] T041 [US3] Verify category color application for compact and regular nodes in `gladius/src/ui/NodeView.cpp`

**Checkpoint**: User Story 3 is independently functional and testable.

---

## Phase 6: User Story 4 - Improved Begin/End Node Usability (Priority: P2)

**Goal**: Make begin/end nodes visually distinct and support clear inline add/rename/remove/reorder interactions.

**Independent Test**: Use only the begin/end nodes of a function to add, rename, reorder, and remove signature rows while confirming links remain coherent.

### Tests for User Story 4

- [ ] T042 [P] [US4] Add begin/end row action tests in `gladius/tests/unittests/NodeView_tests.cpp`
- [ ] T043 [P] [US4] Add reorder and link-preservation tests in `gladius/tests/unittests/NodeView_tests.cpp`

### Implementation for User Story 4

- [ ] T044 [US4] Implement visually distinct begin/end node styling in `gladius/src/ui/NodeView.cpp`
- [ ] T045 [US4] Refine inline add, rename, and remove controls for begin nodes in `gladius/src/ui/NodeView.cpp`
- [ ] T046 [US4] Refine inline add, rename, and remove controls for end nodes in `gladius/src/ui/NodeView.cpp`
- [ ] T047 [US4] Implement discoverable reorder interactions and fallback controls in `gladius/src/ui/NodeView.cpp`
- [ ] T048 [US4] Preserve and update links correctly during reorder and rename flows in `gladius/src/nodes/Model.cpp` and `gladius/src/ui/NodeView.cpp`
- [ ] T049 [US4] Add connected-row removal confirmation behavior in `gladius/src/ui/NodeView.cpp`

**Checkpoint**: User Story 4 is independently functional and testable.

---

## Phase 7: User Story 5 - Fluid Responsiveness During Parameter Editing (Priority: P2)

**Goal**: Keep the editor responsive during rapid parameter edits by coalescing expensive recompute work while preserving immediate UI feedback.

**Independent Test**: Rapidly edit parameters on a non-trivial graph and confirm UI responsiveness, smooth editor interaction, and latest-value-wins behavior.

### Tests for User Story 5

- [ ] T050 [P] [US5] Add throttle timing tests in `gladius/tests/unittests/ParameterThrottle_tests.cpp`
- [ ] T051 [P] [US5] Add latest-value-wins and coalescing tests in `gladius/tests/unittests/ParameterThrottle_tests.cpp`

### Implementation for User Story 5

- [ ] T052 [US5] Implement debounce and coalescing behavior in `gladius/src/ui/ParameterThrottle.cpp`
- [ ] T053 [US5] Integrate parameter throttling into the UI update path in `gladius/src/ui/MainWindow.cpp`
- [ ] T054 [US5] Ensure numeric widgets update UI-visible values immediately while recompute is deferred in `gladius/src/ui/NumericWidgets.cpp` and `gladius/src/ui/NodeView.cpp`
- [ ] T055 [US5] Add responsiveness-safe state reset and edge-case handling in `gladius/src/ui/ParameterThrottle.cpp` and `gladius/src/ui/MainWindow.cpp`

**Checkpoint**: User Story 5 is independently functional and testable.

---

## Phase 8: User Story 6 - Improved Function Input/Output Editing (Priority: P3)

**Goal**: Improve function-call node clarity, navigation, and discoverability of referenced functions.

**Independent Test**: Place function-call nodes, change the referenced function from a searchable list, and navigate to the target function graph without prior knowledge of hidden controls.

### Tests for User Story 6

- [ ] T056 [P] [US6] Add function-call node UI regression coverage in `gladius/tests/unittests/NodeView_tests.cpp`

### Implementation for User Story 6

- [ ] T057 [US6] Promote referenced function name and binding status in `gladius/src/ui/NodeView.cpp`
- [ ] T058 [US6] Add explicit navigation action for referenced functions in `gladius/src/ui/NodeView.cpp` and `gladius/src/ui/ModelEditor.cpp`
- [ ] T059 [US6] Implement searchable function selection flow in `gladius/src/ui/NodeView.cpp`

**Checkpoint**: User Story 6 is independently functional and testable.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Final verification, cleanup, and cross-story refinements.

- [ ] T060 [P] Run focused unit and integration validation for this feature in `gladius/tests/unittests/` and `gladius/tests/integrationtests/`
- [ ] T061 [P] Resolve remaining zero-port, unresolved-type, and zoom-level edge cases in `gladius/src/ui/NodeView.cpp` and `gladius/src/ui/ModelEditor.cpp`
- [ ] T062 Perform visual review and cleanup for compact and regular node consistency in `gladius/src/ui/NodeView.cpp` and `gladius/src/ui/Style.cpp`
- [ ] T063 Validate the manual acceptance flow in `specs/023-node-editor-ux/quickstart.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; can start immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks all user stories.
- **User Stories (Phases 3–8)**: Depend on Foundational completion.
- **Polish (Phase 9)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational; independent MVP slice.
- **User Story 2 (P1)**: Can start after Foundational; independent from US1 aside from the shared pin baseline.
- **User Story 3 (P2)**: Can start after Foundational and should build on the shared pin baseline from US2.
- **User Story 4 (P2)**: Can start after Foundational; independent of US1 and US5.
- **User Story 5 (P2)**: Can start after Foundational; complements US1 but remains independently testable.
- **User Story 6 (P3)**: Can start after Foundational; independent of other stories.

### Within Each User Story

- Tests first, then helper or state changes, then UI integration, then validation.
- Shared helper or module tasks must complete before integration tasks that consume them.
- Each story ends at an independent validation checkpoint.

### Parallel Opportunities

- Setup tasks `T002`–`T007` can run in parallel.
- Foundational tasks `T011`, `T012`, and `T014` can run in parallel after `T009` and `T010` start.
- Test-writing tasks for each story marked `[P]` can run in parallel.
- After Foundational, US1 and US2 can proceed in parallel.
- After the shared pin baseline is stable, US3, US4, US5, and US6 can be split across multiple contributors.

---

## Parallel Example: User Story 1

```text
- T015 [P] [US1] Add adaptive sensitivity unit tests in gladius/tests/unittests/NumericWidget_tests.cpp
- T016 [P] [US1] Add bounded/unbounded dial behavior tests in gladius/tests/unittests/NumericWidget_tests.cpp
- T017 [P] [US1] Add layout-mode and synchronization tests in gladius/tests/unittests/NumericWidget_tests.cpp
```

```text
- T021 [US1] Integrate shared numeric widgets into scalar parameter rendering in gladius/src/ui/NodeView.cpp
- T022 [US1] Integrate shared numeric widgets into vector parameter rendering in gladius/src/ui/NodeView.cpp
```

## Parallel Example: User Story 2

```text
- T025 [P] [US2] Add drag-session state and reset tests in gladius/tests/unittests/LinkDragState_tests.cpp
- T026 [P] [US2] Add compatibility resolution tests for static and dynamic types in gladius/tests/unittests/LinkDragState_tests.cpp
- T027 [P] [US2] Add node-view pin highlighting regression tests in gladius/tests/unittests/NodeView_tests.cpp
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Setup and Foundational work.
2. Complete User Story 1.
3. Validate numeric editing independently.
4. Stop and review before expanding scope.

### Incremental Delivery

1. Setup + Foundational
2. US1 → validate
3. US2 → validate
4. US3, US4, US5 in parallel or sequential order as capacity allows → validate each independently
5. US6 → validate
6. Polish and quickstart validation

### Parallel Team Strategy

1. One contributor stabilizes shared pin and style infrastructure.
2. One contributor takes numeric widgets and responsiveness.
3. One contributor takes compact, begin-end, and function-call presentation.
4. Merge only after each story passes its independent validation checkpoint.

---

## Notes

- `[P]` means the task targets different files or isolated test additions and is safe to parallelize.
- `[US1]`…`[US6]` labels map tasks back to independently testable user stories.
- The initial polished pass explicitly excludes true circular perimeter-pin interaction.
- Use VS Code tasks for build and test verification per project guidance.
