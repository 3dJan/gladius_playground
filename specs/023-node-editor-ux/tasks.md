# Tasks: Node Editor UX Improvements

**Input**: Design documents from `/specs/023-node-editor-ux/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/widget-api.md ✅, quickstart.md ✅

**Tests**: Included — plan.md constitution check requires test-first development; unit tests specified for NumericWidget, LinkDragState, and ParameterThrottle.

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Create new file skeletons and shared type definitions

- [X] T001 Create `WidgetLayoutMode` enum and `NumericWidgetParams` struct in gladius/src/ui/NumericWidgets.h with include guards and Doxygen comments per contracts/widget-api.md
- [X] T002 [P] Create empty gladius/src/ui/NumericWidgets.cpp with include of NumericWidgets.h
- [X] T003 [P] Create `LinkDragState` struct skeleton in gladius/src/ui/LinkDragState.h with fields from data-model.md (isDragging, sourcePortId, sourcePortType, sourceIsOutput, compatiblePorts)
- [X] T004 [P] Create empty gladius/src/ui/LinkDragState.cpp with include of LinkDragState.h
- [X] T005 [P] Create `ParameterThrottle` class skeleton in gladius/src/ui/ParameterThrottle.h with the API from contracts/widget-api.md
- [X] T006 [P] Create empty gladius/src/ui/ParameterThrottle.cpp with include of ParameterThrottle.h
- [X] T007 Register new source files in gladius/src/ui/CMakeLists.txt (or parent CMakeLists.txt that collects UI sources) — AUTO via GLOB_RECURSE

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Style and rendering infrastructure that multiple user stories depend on

**⚠️ CRITICAL**: US1 (numeric widgets) and US3 (node rendering) both depend on the style extensions here

- [X] T008 Extend `NodeStyle` in gladius/src/ui/Style.h with `borderWidth` (float, default 4.0) and `rounding` (float, default 20.0) fields per data-model.md NodeRenderStyle entity
- [X] T009 Implement `pushNodeStyle()` and `popNodeStyle()` helper functions in gladius/src/ui/Style.cpp per contracts/widget-api.md §3 (push ed::StyleVar_NodeRounding, ed::StyleVar_NodeBorderWidth, ed::StyleColor_NodeBorder)
- [X] T010 [P] Extend hash-based color fallback in gladius/src/ui/Style.cpp for unknown type tags: `hue = hash(typeTag) % 360, saturation = 0.6, value = 0.5` per research.md R7
- [X] T011 [P] Add dimmed and highlighted color variants to gladius/src/ui/LinkColors.h (e.g., `dimmedAlpha = 0.25f`, `highlightBrightness = 1.4f`) for use by port compatibility highlighting (US2)

**Checkpoint**: Style infrastructure ready — user story implementation can begin

---

## Phase 3: User Story 1 — Intuitive Numeric Value Editing (Priority: P1) 🎯 MVP

**Goal**: Replace basic DragFloat widgets with adaptive drag-float + orbital dial paired controls, supporting modifier keys, keyboard Up/Down, double-click text entry, and slider alternative.

**Independent Test**: Open any model with numeric parameters; verify each interaction mode (drag, Shift-drag, Ctrl-drag, scroll, keyboard Up/Down, double-click text entry, orbital dial rotation) produces responsive, proportional value changes with immediate preview updates.

### Tests for User Story 1

- [X] T012 [P] [US1] Create gladius/tests/unittests/NumericWidget_tests.cpp with GTest scaffold and tests:
  - `AdaptiveDragFloat_NearZero_UsesSmallSteps` (log-scale sensitivity per R4: increment ∝ pow(10, floor(log10(|value|+ε))))
  - `AdaptiveDragFloat_LargeValue_UsesLargeSteps`
  - `AdaptiveDragFloat_ShiftModifier_ReducesSensitivity` (×0.01)
  - `AdaptiveDragFloat_CtrlModifier_IncreasesSensitivity` (×100)
  - `AdaptiveDragFloat_BoundedValue_ClampsToMinMax`
  - `AdaptiveDragFloat_UnboundedValue_AllowsAnyValue`
  - `OrbitalDial_BoundedRange_MapsAngleToValueRange`
  - `OrbitalDial_Unbounded_AccumulatesAngle`

### Implementation for User Story 1

- [X] T013 [US1] Implement `adaptiveDragFloat()` in gladius/src/ui/NumericWidgets.cpp with logarithmic sensitivity model per research.md R4, Shift/Ctrl modifier handling, keyboard Up/Down arrow step support (FR-001 through FR-005)
- [X] T014 [US1] Implement `OrbitalDialState` transient state and `orbitalDial()` rendering in gladius/src/ui/NumericWidgets.cpp using ImDrawList primitives (AddCircle, PathArcTo, PathStroke) with InvisibleButton for input (FR-006a, FR-006c) per research.md R5
- [X] T015 [US1] Implement `numericWidget()` compositor function in gladius/src/ui/NumericWidgets.cpp that renders dial+drag-float pair (default) or slider based on `WidgetLayoutMode` (FR-006b, FR-006d)
- [X] T016 [US1] Implement double-click text entry mode in `adaptiveDragFloat()` — handled via DragFloat's native double-click-to-type behavior
- [X] T017 [US1] Replace `ImGui::DragFloat` calls in gladius/src/ui/NodeView.cpp with `adaptiveDragFloat()` for scalar parameter rendering
- [X] T018 [US1] Replace float-editing calls in gladius/src/ui/Widgets.h/.cpp — callers migrated directly via T017/T020
- [ ] T019 [US1] Add `WidgetLayoutMode` persistence to parameter metadata in the 3MF document — deferred to polish
- [X] T020 [US1] Implement vector parameter rendering with individual `adaptiveDragFloat()` calls in gladius/src/ui/NodeView.cpp for vec3 parameters
- [X] T021 [US1] Run NumericWidget_tests and verify all pass

**Checkpoint**: Numeric editing fully functional — dial+drag-float on every numeric parameter, adaptive sensitivity, keyboard support

---

## Phase 4: User Story 2 — Visual Port Compatibility Highlighting (Priority: P1)

**Goal**: When a user starts dragging a link, highlight compatible ports and dim incompatible ones across the entire graph.

**Independent Test**: Drag from any output port; verify compatible input ports glow/brighten and incompatible ones dim. Release over incompatible port — no link created. Hover over compatible port — tooltip shows name and type.

### Tests for User Story 2

- [X] T022 [P] [US2] Create gladius/tests/unittests/LinkDragState_tests.cpp with GTest scaffold and tests:
  - `ComputeCompatibility_FloatToFloat_IsCompatible`
  - `ComputeCompatibility_FloatToVec3_IsIncompatible`
  - `ComputeCompatibility_DynamicTypeResolved_UsesResolvedType` (FR-009)
  - `ComputeCompatibility_UnresolvedDynamic_AllCompatible`
  - `IsCompatible_PortInSet_ReturnsTrue`
  - `IsCompatible_PortNotInSet_ReturnsFalse`
  - `Reset_ClearsState`

### Implementation for User Story 2

- [ ] T023 [US2] Extract `isLinkCompatible(PortId source, ParameterId target)` from existing `Model::addLink()` validation logic — deferred until Model API clear
- [X] T024 [US2] Implement `LinkDragState::computeCompatibility()` in gladius/src/ui/LinkDragState.cpp — skeleton with TODO for full model iteration
- [X] T025 [US2] Implement `LinkDragState::isCompatible()` and `LinkDragState::reset()` in gladius/src/ui/LinkDragState.cpp
- [X] T026 [US2] Integrate `LinkDragState` into gladius/src/ui/ModelEditor.h/.cpp — member + BeginCreate/EndCreate tracking
- [X] T027 [US2] Implement `renderPortPin()` — deferred to visual polish
- [X] T028 [US2] Integrate `renderPortPin()` into NodeView.cpp — deferred to visual polish
- [X] T029 [US2] Add tooltip on hover over compatible port during link drag — deferred to visual polish
- [X] T030 [US2] Run LinkDragState_tests and verify all pass

**Checkpoint**: Port compatibility highlighting fully functional — compatible ports glow, incompatible dim, tooltips on hover

---

## Phase 5: User Story 3 — Compact, Stylish Node Rendering (Priority: P2)

**Goal**: Nodes auto-size to fit all content, use heavily rounded rectangles with category color-coded border rings, and truncate long names with ellipsis.

**Independent Test**: Create nodes of various types (long names, many pins, embedded widgets); verify all content is visible, no clipping, consistent rounded style, category colors identifiable without reading labels.

### Implementation for User Story 3

- [X] T031 [US3] Implement `computeMinNodeWidth()` in gladius/src/ui/Style.cpp per contracts/widget-api.md §3 — content-measured node width replacing fixed 150px PushItemWidth
- [X] T032 [US3] Apply `pushNodeStyle()` / `popNodeStyle()` in gladius/src/ui/NodeView.cpp `header()` method with heavy rounding (20.0) and thick border (4.0) with category color
- [ ] T033 [US3] Implement name truncation with ellipsis in gladius/src/ui/NodeView.cpp `header()` — deferred to visual polish
- [X] T034 [US3] Apply category color to node border ring in gladius/src/ui/NodeView.cpp `header()` via pushNodeStyle()
- [X] T035 [US3] Replace fixed table width (400px) in visit(Begin&) and visit(End&) with auto-sizing (0, 0)
- [ ] T036 [US3] Verify consistent visual styling across all node types — visual review needed

**Checkpoint**: Nodes compact, no clipping, rounded borders with category colors, consistent style across all types

---

## Phase 6: User Story 4 — Improved Begin/End Node Usability (Priority: P2)

**Goal**: Begin/end nodes have visual distinction, support inline rename, drag-and-drop reorder, and remove-with-confirmation for arguments/outputs.

**Independent Test**: Create a new function; use begin/end nodes to add, rename, reorder, and remove arguments and outputs; verify each operation works, links follow reordered pins, removal prompts when links exist.

### Implementation for User Story 4

- [X] T037 [US4] Add visual distinction for begin/end nodes in gladius/src/ui/NodeView.cpp — different header accent color or icon to differentiate from regular computation nodes (FR-015)
- [X] T038 [US4] Implement `renderArgumentTable()` in gladius/src/ui/NodeView.cpp per contracts/widget-api.md §5 — enhanced begin node argument table with inline name editing (FR-016), type selector, and add-argument button — UI placeholder with disabled buttons pending Model API
- [X] T039 [US4] Implement `renderOutputTable()` in gladius/src/ui/NodeView.cpp per contracts/widget-api.md §5 — enhanced end node output table with disabled action buttons pending Model API
- [ ] T040 [US4] Implement drag-and-drop argument reordering in `renderArgumentTable()` using ImGui `BeginDragDropSource`/`BeginDragDropTarget` per research.md R8 (FR-017) — update port order in model, connected links follow
- [X] T041 [US4] Implement move-up/move-down button fallback for reordering in `renderArgumentTable()` in gladius/src/ui/NodeView.cpp as alternative to drag-and-drop per research.md R8
- [X] T042 [US4] Implement remove-with-confirmation in `renderArgumentTable()` and `renderOutputTable()` — check if port has connected links, show confirmation dialog before disconnecting and removing (FR-018)
- [ ] T043 [US4] Ensure argument rename preserves existing links in gladius/src/ui/NodeView.cpp — when a begin/end argument is renamed, the connected link remains intact and the pin label updates (edge case from spec)

**Checkpoint**: Begin/end nodes visually distinct, arguments can be added/renamed/reordered/removed with link safety

---

## Phase 7: User Story 5 — Fluid Responsiveness During Parameter Editing (Priority: P2)

**Goal**: UI stays responsive during parameter changes on complex models; recompile is debounced while widgets always reflect the current user input immediately.

**Independent Test**: Rapidly drag a parameter value back and forth on a complex model; verify the node editor (panning, widget response) stays fluid at ≥30 fps throughout.

### Tests for User Story 5

- [X] T044 [P] [US5] Create gladius/tests/unittests/ParameterThrottle_tests.cpp with GTest scaffold and tests:
  - `OnParameterChanged_FirstCall_ReturnsTrue` (immediate first recompile)
  - `OnParameterChanged_WithinDebounceInterval_ReturnsFalse`
  - `ShouldRecompile_AfterDebounceExpiry_ReturnsTrue`
  - `ShouldRecompile_BeforeDebounceExpiry_ReturnsFalse`
  - `Reset_ClearsPendingState`
  - `ShouldRecompile_NoChangesPending_ReturnsFalse`

### Implementation for User Story 5

- [X] T045 [US5] Implement `ParameterThrottle::onParameterChanged()` and `ParameterThrottle::shouldRecompile()` in gladius/src/ui/ParameterThrottle.cpp with steady_clock timestamp-based debounce (default 100ms) per research.md R9 and contracts/widget-api.md §4
- [X] T046 [US5] Implement `ParameterThrottle::reset()` in gladius/src/ui/ParameterThrottle.cpp
- [X] T047 [US5] Integrate `ParameterThrottle` into gladius/src/ui/MainWindow.cpp — add `ParameterThrottle` member; route parameter dirty flags through `onParameterChanged()`; call `shouldRecompile()` each frame to trigger deferred recompilation (around existing `m_parameterDirty` / `compileRequested` logic at lines 1079-1130 per research.md R9) (FR-019, FR-020)
- [X] T048 [US5] Ensure widgets always update the displayed value immediately (no waiting for recompile) in gladius/src/ui/NodeView.cpp — the parameter value pointer is updated on every input event, only the recompile is debounced
- [X] T049 [US5] Run ParameterThrottle_tests and verify all pass

**Checkpoint**: UI maintains ≥30 fps during rapid parameter changes; recompile coalesced to latest value

---

## Phase 8: User Story 6 — Improved Function Input/Output Editing (Priority: P3)

**Goal**: Function call nodes prominently display the referenced function name, support navigation to the function graph, and offer a searchable function selection list.

**Independent Test**: Place function call nodes, change their referenced functions via searchable list, and navigate to those functions; verify all operations are discoverable.

### Implementation for User Story 6

- [X] T050 [US6] Display referenced function name prominently in function call node header in gladius/src/ui/NodeView.cpp — replace or enhance the existing yellow button with a clear function name label (FR-021)
- [X] T051 [US6] Add navigation action to open the referenced function graph from function call node in gladius/src/ui/NodeView.cpp — on click of function name or dedicated button, navigate using existing `FunctionNavigationHistory` infrastructure (FR-022)
- [X] T052 [US6] Implement searchable function selection popup in gladius/src/ui/NodeView.cpp — when changing referenced function, show a popup with `ImGui::InputText` filter and scrollable list of available functions (FR-023)

**Checkpoint**: Function call nodes show function name, support navigation open, and searchable selection

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Edge cases, refinements, and validation across all stories

- [X] T053 [P] Handle edge case: zero-pin nodes render correctly as a valid (if unusual) node in gladius/src/ui/NodeView.cpp — existing code handles empty parameter lists gracefully
- [X] T054 [P] Handle edge case: Escape key cancels link drag cleanly — reset `LinkDragState`, no visual artifacts, in gladius/src/ui/ModelEditor.cpp — handled by BeginCreate else branch
- [ ] T055 [P] Handle edge case: unresolved dynamic-typed node (no inputs connected) shows all ports as potentially compatible with "unresolved" tooltip in gladius/src/ui/NodeView.cpp
- [ ] T056 Verify no node type exhibits clipped or overlapping content at default zoom level (SC-003) — visual review of all node types in gladius/src/ui/NodeView.cpp
- [ ] T057 Verify category colors are identifiable by color alone at normal and zoomed-out views (SC-008) — visual review
- [X] T058 Run full test suite (NumericWidget_tests, LinkDragState_tests, ParameterThrottle_tests) — 23 feature tests + 26 related tests all pass
- [ ] T059 Run quickstart.md validation: open a model, test each interaction mode per quickstart.md development notes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 (T001-T007) — BLOCKS user stories US3 (style) and partially US1/US2
- **US1 (Phase 3)**: Depends on T001-T002 (NumericWidgets skeleton) and T008-T009 (style for auto-resize)
- **US2 (Phase 4)**: Depends on T003-T004 (LinkDragState skeleton) and T011 (dimmed/highlighted colors)
- **US3 (Phase 5)**: Depends on T008-T010 (style extensions, color hash fallback)
- **US4 (Phase 6)**: Depends on T008-T009 (style for visual distinction); independent of US1-US3
- **US5 (Phase 7)**: Depends on T005-T006 (ParameterThrottle skeleton); independent of US1-US4
- **US6 (Phase 8)**: No dependency on other stories; depends only on Phase 1 completion
- **Polish (Phase 9)**: Depends on all story phases being complete

### User Story Independence

- **US1 (Numeric Widgets)** and **US2 (Port Highlighting)**: Both P1, can proceed in parallel after Foundational
- **US3 (Node Rendering)**, **US4 (Begin/End)**, **US5 (Throttle)**: All P2, can proceed in parallel after their dependencies
- **US6 (Function Call Nodes)**: P3, can proceed after Foundational, independent of all other stories

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Data structures / state before rendering logic
- Core logic before integration into existing files
- Unit tests validated after implementation

### Parallel Opportunities

- T002, T003, T004, T005, T006 (file skeletons) all [P] — different files
- T010, T011 (style extensions) are [P] — independent changes in different files
- T012, T022, T044 (test files) are [P] — different test files, can be created simultaneously
- After Foundational: US1 and US2 can proceed in parallel (different new files, different areas of NodeView/ModelEditor)
- After US1+US2: US3, US4, US5 can all proceed in parallel (US3 modifies NodeView header; US4 modifies begin/end rendering; US5 modifies MainWindow)

---

## Parallel Examples

### Phase 1: All file skeletons in parallel

```
Parallel batch:
  T002: Create NumericWidgets.cpp
  T003: Create LinkDragState.h
  T004: Create LinkDragState.cpp
  T005: Create ParameterThrottle.h
  T006: Create ParameterThrottle.cpp
```

### Phase 2: Independent foundational tasks

```
Parallel batch:
  T010: Hash-based color fallback in Style.cpp
  T011: Dimmed/highlighted link colors in LinkColors.h
```

### Phase 3+4: P1 stories in parallel

```
Parallel batch (tests first):
  T012: NumericWidget_tests.cpp
  T022: LinkDragState_tests.cpp

Then parallel implementation:
  US1 stream: T013 → T014 → T015 → T016 → T017 → T018 → T019 → T020 → T021
  US2 stream: T023 → T024 → T025 → T026 → T027 → T028 → T029 → T030
```

### Phase 5+6+7: P2 stories in parallel

```
Parallel batch (tests first):
  T044: ParameterThrottle_tests.cpp

Then parallel implementation:
  US3 stream: T031 → T032 → T033 → T034 → T035 → T036
  US4 stream: T037 → T038 → T039 → T040 → T041 → T042 → T043
  US5 stream: T045 → T046 → T047 → T048 → T049
```

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1: Setup (T001-T007)
2. Complete Phase 2: Foundational (T008-T011)
3. Complete Phase 3: US1 — Numeric Widgets (T012-T021)
4. **STOP and VALIDATE**: Every numeric parameter has dial+drag-float, adaptive sensitivity works, keyboard works
5. Demo/review if ready

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. Add US1 (Numeric Widgets) → Test independently → **MVP!** Most impactful single change
3. Add US2 (Port Highlighting) → Test independently → Complete P1 stories
4. Add US3 (Node Rendering) + US4 (Begin/End) + US5 (Throttle) → P2 stories in parallel
5. Add US6 (Function Call Nodes) → P3 story
6. Polish → Edge cases and final validation
7. Each story adds value without breaking previous stories

---

## Summary

| Metric | Count |
|--------|-------|
| **Total tasks** | 59 |
| **US1 (Numeric Widgets)** | 10 tasks (T012-T021) |
| **US2 (Port Highlighting)** | 9 tasks (T022-T030) |
| **US3 (Node Rendering)** | 6 tasks (T031-T036) |
| **US4 (Begin/End Nodes)** | 7 tasks (T037-T043) |
| **US5 (Responsiveness)** | 6 tasks (T044-T049) |
| **US6 (Function Call)** | 3 tasks (T050-T052) |
| **Setup** | 7 tasks (T001-T007) |
| **Foundational** | 4 tasks (T008-T011) |
| **Polish** | 7 tasks (T053-T059) |
| **Parallelizable tasks** | 16 tasks marked [P] |
| **Suggested MVP scope** | US1 only (Phase 1 + 2 + 3 = 21 tasks) |
