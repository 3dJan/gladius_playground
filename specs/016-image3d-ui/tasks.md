````markdown
# Tasks: Image3D & FunctionFromImage3D UI

**Input**: Design documents from `/specs/016-image3d-ui/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/api.md ✓

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US7)

---

## Phase 1: Setup

**Purpose**: Project structure and header stubs

- [X] T001 Create header stub for ImageStackView in gladius/src/ui/ImageStackView.h
- [X] T002 [P] Create header stub for FunctionFromImage3DView in gladius/src/ui/FunctionFromImage3DView.h
- [X] T003 [P] Add ImageStackViewState and FunctionFromImage3DViewState structs to gladius/src/ui/ViewStates.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: These transform methods and indexed access are used by US1, US6, and US7

- [X] T004 [P] Add at(index) accessor method to ImageStack in gladius/src/io/3mf/ImageStack.h
- [X] T005 [P] Implement flipHorizontal() method on Image class in gladius/src/io/3mf/ImageStack.h
- [X] T006 [P] Implement flipVertical() method on Image class in gladius/src/io/3mf/ImageStack.h
- [X] T007 [P] Implement rotate90CW() method on Image class in gladius/src/io/3mf/ImageStack.h
- [X] T008 [P] Implement rotate90CCW() method on Image class in gladius/src/io/3mf/ImageStack.h
- [X] T009 [P] Implement padTo() method on Image class in gladius/src/io/3mf/ImageStack.h
- [X] T010 [P] Add unit tests for Image transform methods in gladius/tests/unittests/ImageTransform_Test.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - View and Navigate ImageStack Layers (Priority: P1) 🎯 MVP

**Goal**: Display 2D layer viewer with slider navigation for ImageStack resources

**Independent Test**: Load 3MF with ImageStack → select in Outline → verify layer viewer shows → navigate layers with slider

**Requirements**: FR-001, FR-002, FR-003, FR-004, FR-005

### Tests for User Story 1

- [X] T011 [P] [US1] Create integration test for ImageStackView in gladius/tests/unittests/ImageStackView_Test.cpp

### Implementation for User Story 1

- [X] T012 [US1] Implement ImageStackView constructor and destructor in gladius/src/ui/ImageStackView.cpp
- [X] T013 [US1] Implement setImageStack() accepting ImageStack pointer in gladius/src/ui/ImageStackView.cpp
- [X] T014 [US1] Implement layer texture upload to OpenGL in gladius/src/ui/ImageStackView.cpp
- [X] T015 [US1] Implement render() with ImGui::Image for current layer in gladius/src/ui/ImageStackView.cpp
- [X] T016 [US1] Add ImGui::SliderInt for layer navigation showing "Layer X of Y" in gladius/src/ui/ImageStackView.cpp
- [X] T017 [US1] Add mouse wheel handler for layer scrolling when isHovered() in gladius/src/ui/ImageStackView.cpp
- [X] T018 [US1] Implement aspect-ratio-preserving scaling to fit panel in gladius/src/ui/ImageStackView.cpp
- [X] T019 [US1] Integrate ImageStackView with ResourceView for ImageStack selection in gladius/src/ui/ResourceView.cpp
- [X] T020 [US1] Add ImageStack resources to Outline tree view in gladius/src/ui/Outline.cpp

**Checkpoint**: US1 complete - ImageStack layers viewable and navigable

---

## Phase 4: User Story 2 - Configure FunctionFromImage3D Settings (Priority: P1)

**Goal**: Tabbed Properties/Graph interface for configuring FunctionFromImage3D sampling parameters

**Independent Test**: Select FunctionFromImage3D in Outline → verify tabbed panel opens → change filter/tile settings → verify settings persist

**Requirements**: FR-006, FR-007, FR-008, FR-009, FR-010, FR-012, FR-024, FR-025

### Tests for User Story 2

- [X] T021 [P] [US2] Create integration test for FunctionFromImage3DView in gladius/tests/unittests/FunctionFromImage3DView_Test.cpp

### Implementation for User Story 2

- [X] T022 [US2] Implement FunctionFromImage3DView constructor and destructor in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T023 [US2] Implement setFunction() and setModelEditor() in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T024 [US2] Implement findImageSampler() to locate ImageSampler node in function graph in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T025 [US2] Add isFunctionFromImage3D() detection method to ModelEditor in gladius/src/ui/ModelEditor.cpp
- [X] T026 [US2] Add TabMode enum and tab switching UI to ModelEditor in gladius/src/ui/ModelEditor.cpp
- [X] T027 [US2] Render Properties tab with ImGui::BeginTabBar in when FunctionFromImage3D detected in gladius/src/ui/ModelEditor.cpp
- [X] T028 [US2] Implement filter mode ImGui::Combo (Linear/Nearest) in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T029 [US2] Implement tile style ImGui::Combo for U axis (Wrap/Mirror/Clamp) in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T030 [US2] Implement tile style ImGui::Combo for V axis (Wrap/Mirror/Clamp) in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T031 [US2] Implement tile style ImGui::Combo for W axis (Wrap/Mirror/Clamp) in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T032 [US2] Implement offset ImGui::DragFloat with 0.01 speed in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T033 [US2] Implement scale ImGui::DragFloat with 0.01 speed in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T034 [US2] Add createUndoRestorePoint() call before each setting change in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T035 [US2] Call markModelAsModified() after setting changes to trigger recompile in gladius/src/ui/FunctionFromImage3DView.cpp

**Checkpoint**: US2 complete - FunctionFromImage3D fully configurable via Properties panel

---

## Phase 5: User Story 3 - Select ImageStack for FunctionFromImage3D (Priority: P2)

**Goal**: Visual dropdown selector to choose which ImageStack the function references

**Independent Test**: Open FunctionFromImage3D panel → use selector to change ImageStack → verify function updates reference

**Requirements**: FR-011

### Implementation for User Story 3

- [X] T036 [US3] Enumerate available ImageStack resources from Assembly in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T037 [US3] Implement ImGui::Combo for ImageStack selection with resource IDs in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T038 [US3] Update Resource node in function graph when selection changes in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T039 [US3] Show "No ImageStacks available" message when Assembly has none in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T040 [US3] Add tooltip preview showing first layer of ImageStack on hover in gladius/src/ui/FunctionFromImage3DView.cpp

**Checkpoint**: US3 complete - ImageStack can be selected/changed for function

---

## Phase 6: User Story 4 - Preview FunctionFromImage3D Output (Priority: P2)

**Goal**: 2D slice preview showing cross-section of function output at controllable position

**Independent Test**: Open FunctionFromImage3D panel → verify preview shows → change settings → preview updates within 500ms

**Requirements**: FR-013, FR-014, FR-015

### Implementation for User Story 4

- [X] T041 [US4] Add preview texture and dirty flag to FunctionFromImage3DViewState in gladius/src/ui/ViewStates.h
- [X] T042 [US4] Implement updatePreviewTexture() sampling function at slice position in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T043 [US4] Implement renderPreview() displaying ImGui::Image of slice in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T044 [US4] Add slice position ImGui::SliderFloat with range -0.5 to 1.5 for tile demo in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T045 [US4] Invalidate preview when any configuration setting changes in gladius/src/ui/FunctionFromImage3DView.cpp
- [X] T046 [US4] Throttle preview updates to max 500ms between changes in gladius/src/ui/FunctionFromImage3DView.cpp

**Checkpoint**: US4 complete - live 2D preview visible and responsive

---

## Phase 7: User Story 5 - Create New ImageStack from Image Directory (Priority: P2)

**Goal**: Import PNG directory as ImageStack with dimension handling for varying sizes

**Independent Test**: Select directory of mixed-size PNGs → import → verify ImageStack created with correct dimensions → verify padding notification

**Requirements**: FR-016, FR-017, FR-018, FR-026

### Implementation for User Story 5

- [X] T047 [US5] Define ImportProgressCallback signature in gladius/src/io/3mf/ImageStackCreator.h
- [X] T048 [US5] Add progress callback parameter to addImageStackFromDirectory() in gladius/src/io/3mf/ImageStackCreator.cpp
- [X] T049 [US5] Detect maximum dimensions across all images before import in gladius/src/io/3mf/ImageStackCreator.cpp
- [X] T050 [US5] Apply padTo() for images smaller than max dimensions in gladius/src/io/3mf/ImageStackCreator.cpp
- [X] T051 [US5] Collect list of padded filenames for notification in gladius/src/io/3mf/ImageStackCreator.cpp
- [X] T052 [US5] Show notification dialog listing which images were padded in gladius/src/ui/ResourceView.cpp
- [X] T053 [US5] Add "Import ImageStack from Directory" menu item in gladius/src/ui/MainMenu.cpp

**Checkpoint**: US5 complete - Directory import with padding working

---

## Phase 8: User Story 6 - Basic ImageStack Transforms (Priority: P3)

**Goal**: Flip and rotate transforms applicable to entire ImageStack with undo support

**Independent Test**: Select ImageStack → apply flip horizontal → verify all layers flipped → Ctrl+Z undoes transform

**Requirements**: FR-019, FR-020, FR-021, FR-024

### Tests for User Story 6

- [X] T054 [P] [US6] Create unit tests for ImageStack transforms in gladius/tests/unittests/ImageStackTransform_Test.cpp

### Implementation for User Story 6

- [X] T055 [US6] Implement flipHorizontal() on ImageStack delegating to each Image in gladius/src/io/3mf/ImageStack.cpp
- [X] T056 [US6] Implement flipVertical() on ImageStack delegating to each Image in gladius/src/io/3mf/ImageStack.cpp
- [X] T057 [US6] Implement rotate90CW() on ImageStack delegating to each Image in gladius/src/io/3mf/ImageStack.cpp
- [X] T058 [US6] Implement rotate90CCW() on ImageStack delegating to each Image in gladius/src/io/3mf/ImageStack.cpp
- [X] T059 [US6] Add transform buttons (Flip H, Flip V, Rotate CW, Rotate CCW) to ImageStackView in gladius/src/ui/ImageStackView.cpp
- [X] T060 [US6] Create undo restore point before each transform in gladius/src/ui/ImageStackView.cpp
- [X] T061 [US6] Refresh layer texture after transform completes in gladius/src/ui/ImageStackView.cpp

**Checkpoint**: US6 complete - Transforms with undo working

---

## Phase 9: User Story 7 - Create FunctionFromImage3D (Priority: P3)

**Goal**: Right-click workflow to create FunctionFromImage3D from existing ImageStack

**Independent Test**: Right-click ImageStack in Outline → select Create FunctionFromImage3D → verify function created with defaults → Properties panel opens

**Requirements**: FR-022, FR-023

### Implementation for User Story 7

- [X] T062 [US7] Add context menu to ImageStack items in Outline with "Create FunctionFromImage3D" option in gladius/src/ui/Outline.cpp
- [X] T063 [US7] Call Builder::createFunctionFromImage3D() with selected ImageStack's ResourceId in gladius/src/ui/Outline.cpp
- [X] T064 [US7] Initialize new function with default values (Linear, Wrap, scale=1, offset=0) in gladius/src/ui/Outline.cpp
- [X] T065 [US7] Select newly created function in Outline to open Properties panel in gladius/src/ui/Outline.cpp

**Checkpoint**: US7 complete - Creation workflow working

---

## Phase 10: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, validation, and cleanup

- [X] T066 [P] Add Doxygen comments to ImageStackView public API in gladius/src/ui/ImageStackView.h
- [X] T067 [P] Add Doxygen comments to FunctionFromImage3DView public API in gladius/src/ui/FunctionFromImage3DView.h
- [X] T068 [P] Add Doxygen comments to ImageStack transform methods in gladius/src/io/3mf/ImageStack.h
- [ ] T069 Run quickstart.md validation scenarios end-to-end
- [ ] T070 Verify success criteria SC-001 through SC-006 from spec.md

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Stories (Phases 3-9)**: All depend on Foundational phase completion
- **Polish (Phase 10)**: Depends on all user stories being complete

### User Story Dependencies

| Story | Priority | Depends On | Can Parallel With |
|-------|----------|------------|-------------------|
| US1 (View Layers) | P1 | Foundational | US2 |
| US2 (Config Panel) | P1 | Foundational | US1 |
| US3 (ImageStack Selector) | P2 | US2 | US4, US5 |
| US4 (Preview) | P2 | US2 | US3, US5 |
| US5 (Import Enhancement) | P2 | Foundational | US3, US4 |
| US6 (Transforms) | P3 | US1, Foundational | US7 |
| US7 (Creation) | P3 | US1, US2 | US6 |

### Within Each User Story

1. Tests first (where specified)
2. Core implementation
3. UI integration
4. Undo support (where applicable)

---

## Parallel Execution Example: Phase 2 (Foundational)

```bash
# All transform methods can be implemented in parallel (different methods):
Task T005: "Implement flipHorizontal() method on Image class"
Task T006: "Implement flipVertical() method on Image class"
Task T007: "Implement rotate90CW() method on Image class"
Task T008: "Implement rotate90CCW() method on Image class"
Task T009: "Implement padTo() method on Image class"
```

## Parallel Execution Example: P1 User Stories

```bash
# US1 and US2 can run in parallel after foundational:
Task T012-T020: "User Story 1 - ImageStackView"
Task T022-T035: "User Story 2 - FunctionFromImage3DView"
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T010)
3. Complete Phase 3: US1 - View Layers (T011-T020)
4. Complete Phase 4: US2 - Config Panel (T021-T035)
5. **STOP and VALIDATE**: Both P1 stories should work independently
6. Deploy/demo if ready

### Incremental Delivery

| Increment | Stories | Value Delivered |
|-----------|---------|-----------------|
| 1 (MVP) | US1 + US2 | View layers, configure sampling |
| 2 | + US3, US4, US5 | ImageStack selection, preview, import |
| 3 | + US6, US7 | Transforms, creation workflow |

---

## Summary

| Metric | Value |
|--------|-------|
| Total Tasks | 70 |
| Setup Tasks | 3 |
| Foundational Tasks | 7 |
| US1 Tasks | 10 |
| US2 Tasks | 15 |
| US3 Tasks | 5 |
| US4 Tasks | 6 |
| US5 Tasks | 7 |
| US6 Tasks | 8 |
| US7 Tasks | 4 |
| Polish Tasks | 5 |
| Parallelizable Tasks | 18 |
| MVP Scope | US1 + US2 (28 tasks) |

````