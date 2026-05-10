# Tasks: Default Mesh Color Export

**Input**: Design documents from `/specs/025-mesh-color-export/`
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Include unit and integration tests for this feature because the plan and constitution require test-first coverage for the planner, quantizer, writer, exporter, and UI-facing warning behavior.

**Organization**: Tasks are grouped by user story so each story can be implemented and validated independently.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the new helper and test files required by the feature.

- [X] T001 Create planner skeleton files in `gladius/src/io/3mf/ColorCompatibilityPlanner.h` and `gladius/src/io/3mf/ColorCompatibilityPlanner.cpp`
- [X] T002 [P] Create quantizer skeleton files in `gladius/src/io/3mf/ColorQuantizer.h` and `gladius/src/io/3mf/ColorQuantizer.cpp`
- [X] T003 [P] Create regionizer skeleton files in `gladius/src/io/3mf/ColorRegionizer.h` and `gladius/src/io/3mf/ColorRegionizer.cpp`
- [X] T004 [P] Create planner and quantizer unit-test skeletons in `gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp` and `gladius/tests/unittests/io/3mf/ColorQuantizer_tests.cpp`
- [X] T005 [P] Create exporter settings and warning test skeletons in `gladius/tests/unittests/MeshExporter3mf_tests.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define the shared settings, data contracts, and writer/exporter seams used by every story.

**⚠️ CRITICAL**: No user story work should begin until this phase is complete.

- [X] T006 Add `QuantizationMode`, `TargetApplication`, and `MeshColorExportSettings` declarations to `gladius/src/io/MeshExporter3mf.h`
- [X] T007 [P] Add `CompatibilityProfile`, `CompatibilityDecision`, and `ExportRepresentation` declarations to `gladius/src/io/3mf/ColorCompatibilityPlanner.h`
- [X] T008 [P] Add `QuantizedPalette` and `PrintableRegion` data types to `gladius/src/io/3mf/ColorQuantizer.h` and `gladius/src/io/3mf/ColorRegionizer.h`
- [X] T009 Add immutable settings snapshot capture and validation to `gladius/src/io/MeshExporter3mf.cpp`
- [X] T010 [P] Add triangle, discrete, build-item, and target-tagged writer interfaces to `gladius/src/io/3mf/MeshWriter3mf.h` and `gladius/src/io/3mf/Writer3mfBase.h`
- [X] T011 [P] Add foundational settings and decision contract coverage in `gladius/tests/unittests/MeshExporter3mf_tests.cpp` and `gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp`

**Checkpoint**: Shared types, export settings, and writer/exporter seams are ready for user-story work.

---

## Phase 3: User Story 1 - Default export produces printable color regions in target slicers (Priority: P1) 🎯 MVP

**Goal**: Make the default colored mesh export preserve printable material or extruder regions in PrusaSlicer and Orca using standards-first fallback behavior.

**Independent Test**: Export colored reference models through the default 3MF mesh workflow and verify that PrusaSlicer and Orca expose distinct printable regions, while uncolored models still export normally.

### Tests for User Story 1 ⚠️

> **NOTE: Write these tests first, and verify that they fail before implementation.**

- [X] T012 [P] [US1] Add planner tests for the canonical `Texture → Vertex → Triangle → Component/Object → Build Item` order in `gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp`
- [X] T013 [P] [US1] Add writer readback tests for triangle, discrete, and build-item standards-only exports in `gladius/tests/unittests/MeshWriter3mfColor_tests.cpp`
- [X] T014 [P] [US1] Add integration tests for printable-region imports, multipart meshes, and uncolored fallback in `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`

### Implementation for User Story 1

- [X] T015 [US1] Implement the canonical standards-first decision ladder in `gladius/src/io/3mf/ColorCompatibilityPlanner.cpp`
- [X] T016 [P] [US1] Implement triangle-to-region grouping and multipart region partitioning in `gladius/src/io/3mf/ColorRegionizer.cpp`
- [X] T017 [US1] Implement the standard triangle-color writer path in `gladius/src/io/3mf/MeshWriter3mf.cpp`
- [X] T018 [US1] Implement standard discrete component/object and build-item fallback writer paths in `gladius/src/io/3mf/MeshWriter3mf.cpp`
- [X] T019 [US1] Wire planner, regionizer, and standards-only fallback dispatch into `gladius/src/io/MeshExporter3mf.cpp`

**Checkpoint**: The default export can preserve printable regions using only standard 3MF output and still handles models without color data.

---

## Phase 4: User Story 2 - Highest compatible printable detail is preserved (Priority: P2)

**Goal**: Preserve the most detailed printable color representation that remains compatible, including deterministic adaptive quantization when simplification is required.

**Independent Test**: Export gradient and repeated-color reference models and verify that the exporter chooses the highest-fidelity compatible representation with deterministic palette reuse.

### Tests for User Story 2 ⚠️

- [X] T020 [P] [US2] Add deterministic quantization and repeated-color reuse tests in `gladius/tests/unittests/io/3mf/ColorQuantizer_tests.cpp`
- [X] T021 [P] [US2] Add integration tests for highest-fidelity compatible selection and gradient fallback in `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`

### Implementation for User Story 2

- [X] T022 [US2] Implement deterministic adaptive palette generation and max-error reporting in `gladius/src/io/3mf/ColorQuantizer.cpp`
- [X] T023 [US2] Extend quantization triggers, palette-limit handling, and fidelity-reduction warnings in `gladius/src/io/3mf/ColorCompatibilityPlanner.cpp`
- [X] T024 [US2] Apply quantized palette reuse across face, vertex, and triangle export paths in `gladius/src/io/3mf/MeshWriter3mf.cpp`
- [X] T025 [US2] Surface fidelity-reduction warnings and export-result reporting in `gladius/src/io/MeshExporter3mf.cpp`
- [X] T026 [US2] Ignore alpha for printable-region planning and emit transparency-loss warnings in `gladius/src/io/3mf/ColorCompatibilityPlanner.cpp` and `gladius/src/io/MeshExporter3mf.cpp`

**Checkpoint**: Compatible exports retain the highest practical detail, and deterministic quantization plus transparency handling are validated by tests.

---

## Phase 5: User Story 3 - Default export remains a raw mesh color workflow (Priority: P3)

**Goal**: Keep the default color workflow on the existing mesh export path and prevent shell export from becoming a hidden fallback.

**Independent Test**: Export a colored model through the default workflow and verify that the output comes from the mesh color pipeline rather than shell-derived geometry.

### Tests for User Story 3 ⚠️

- [X] T027 [P] [US3] Add integration tests proving default colored export never routes through shell export in `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`

### Implementation for User Story 3

- [X] T028 [US3] Keep default color export orchestration in `gladius/src/io/MeshExporter3mf.cpp` and avoid fallback logic in `gladius/src/io/ShellExporter.cpp`
- [X] T029 [US3] Preserve raw mesh-color wording and non-shell behavior in `gladius/src/ui/MeshExportDialog.cpp`
- [X] T030 [US3] Document the non-shell default workflow guarantee in `specs/025-mesh-color-export/quickstart.md`

**Checkpoint**: The default export path stays on direct mesh color handling, with shell export remaining a separate workflow.

---

## Phase 6: User Story 4 - Export dialog allows compatibility tuning (Priority: P3)

**Goal**: Expose quantization behavior and palette controls in the main export dialog and pass them deterministically into the exporter.

**Independent Test**: Open the export dialog for a colored model, adjust quantization settings, and verify that the exporter snapshot reflects the selected values.

### Tests for User Story 4 ⚠️

- [X] T031 [P] [US4] Add settings snapshot and quantization validation tests in `gladius/tests/unittests/MeshExporter3mf_tests.cpp`

### Implementation for User Story 4

- [X] T032 [US4] Add quantization mode and palette override state to `gladius/src/ui/MeshExportDialog.h`
- [X] T033 [US4] Add quantization controls and compatibility hints to `gladius/src/ui/MeshExportDialog.cpp`
- [X] T034 [US4] Add `setQuantizationMode` and `setMaxPaletteSize` plumbing to `gladius/src/io/MeshExporter3mf.h` and `gladius/src/io/MeshExporter3mf.cpp`
- [X] T035 [US4] Wire quantization settings from `gladius/src/ui/MeshExportDialog.cpp` into `gladius/src/io/MeshExporter3mf.cpp`

**Checkpoint**: Users can tune compatibility-driven quantization from the existing export dialog, and the exporter captures the settings immutably.

---

## Phase 7: User Story 5 - Optional target-application mode handles non-standard cases (Priority: P3)

**Goal**: Allow explicit target-application optimization when standard-only export cannot preserve printable regions, without affecting the default portable path.

**Independent Test**: Select a target application for a fallback reference model and verify that the export emits only the chosen target’s proprietary behavior while default exports remain standards-only.

### Tests for User Story 5 ⚠️

- [X] T036 [P] [US5] Add planner tests for no-target standards-limited decisions and explicit target gating in `gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp`
- [X] T037 [P] [US5] Add integration tests for explicit target export and no-target warning behavior in `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`

### Implementation for User Story 5

- [X] T038 [US5] Add target-application state and portability warning UI to `gladius/src/ui/MeshExportDialog.h` and `gladius/src/ui/MeshExportDialog.cpp`
- [X] T039 [US5] Add `setTargetApplication` plumbing and standards-limited warning handling to `gladius/src/io/MeshExporter3mf.h` and `gladius/src/io/MeshExporter3mf.cpp`
- [X] T040 [US5] Implement optional target-specific metadata and tagging hooks in `gladius/src/io/3mf/Writer3mfBase.h` and `gladius/src/io/3mf/Writer3mfBase.cpp`
- [X] T041 [US5] Route proprietary target-tagged export decisions through `gladius/src/io/3mf/MeshWriter3mf.cpp`

**Checkpoint**: Proprietary optimization is explicit, isolated to the selected target, and never enabled silently.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Finish documentation, validation coverage, and end-to-end verification.

- [X] T042 [P] Update validation steps and slicer compatibility matrix guidance in `specs/025-mesh-color-export/quickstart.md`
- [X] T043 [P] Update settings and decision contracts in `specs/025-mesh-color-export/contracts/MeshColorExportSettings.api.md` and `specs/025-mesh-color-export/contracts/CompatibilityDecision.api.md`
- [X] T044 [P] Add async responsiveness, progress, and warning-delivery coverage in `gladius/tests/unittests/MeshExporter3mf_tests.cpp` and `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`
- [X] T045 [P] Update public API comments in `gladius/src/io/MeshExporter3mf.h`, `gladius/src/io/3mf/MeshWriter3mf.h`, and `gladius/src/io/3mf/Writer3mfBase.h`
- [X] T046 Record unit, integration, and manual slicer verification results in `specs/025-mesh-color-export/quickstart.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; start immediately.
- **Foundational (Phase 2)**: Depends on Setup and blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational; establishes the MVP standards-only export path.
- **User Story 2 (Phase 4)**: Depends on Foundational and builds on the planner/writer flow completed in User Story 1.
- **User Story 3 (Phase 5)**: Depends on Foundational; can be implemented after User Story 1 wires the default export path.
- **User Story 4 (Phase 6)**: Depends on Foundational; can proceed once exporter settings plumbing exists.
- **User Story 5 (Phase 7)**: Depends on Foundational and the planner/exporter contracts from User Story 1.
- **Polish (Phase 8)**: Depends on the user stories selected for delivery.

### User Story Dependencies

- **US1 (P1)**: No story dependency beyond Foundational; this is the MVP.
- **US2 (P2)**: Uses the planner/writer flow from US1 but remains independently testable with gradient and palette reference models.
- **US3 (P3)**: Verifies the default path stays on mesh export and does not require US2 or US5.
- **US4 (P3)**: Uses the shared settings snapshot from Foundational and can be validated independently from proprietary tagging.
- **US5 (P3)**: Uses the same planner/exporter foundation as US1 and adds explicit opt-in target behavior.

### Within Each User Story

- Tests must be written before implementation and should fail first.
- Planner and quantizer logic should land before writer/exporter integration that consumes them.
- Exporter wiring should land before UI wiring that depends on the new setters.
- Each story should reach its checkpoint before starting the next dependent story.

### Parallel Opportunities

- `T002`, `T003`, `T004`, and `T005` can run in parallel after `T001` starts the helper structure.
- `T007`, `T008`, `T010`, and `T011` can run in parallel once `T006` defines the shared exporter settings direction.
- In **US1**, `T012`, `T013`, `T014`, and `T016` can run in parallel.
- In **US2**, `T020` and `T021` can run in parallel before `T022`.
- In **US3**, `T027` can run in parallel with documentation prep for `T030`.
- In **US4**, `T031` can run in parallel with `T032`.
- In **US5**, `T036` and `T037` can run in parallel before `T038`-`T041`.

---

## Parallel Example: User Story 1

```text
Task: T012 Add planner tests for the canonical order in gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp
Task: T013 Add writer readback tests for standards-only exports in gladius/tests/unittests/MeshWriter3mfColor_tests.cpp
Task: T014 Add integration tests for printable-region imports in gladius/tests/integrationtests/ColorExport_Integration_tests.cpp
Task: T016 Implement triangle-to-region grouping in gladius/src/io/3mf/ColorRegionizer.cpp
```

## Parallel Example: User Story 2

```text
Task: T020 Add deterministic quantization tests in gladius/tests/unittests/io/3mf/ColorQuantizer_tests.cpp
Task: T021 Add integration tests for highest-fidelity compatible selection in gladius/tests/integrationtests/ColorExport_Integration_tests.cpp
```

## Parallel Example: User Story 3

```text
Task: T027 Add non-shell integration tests in gladius/tests/integrationtests/ColorExport_Integration_tests.cpp
Task: T030 Document the non-shell default workflow guarantee in specs/025-mesh-color-export/quickstart.md
```

## Parallel Example: User Story 4

```text
Task: T031 Add settings snapshot and quantization validation tests in gladius/tests/unittests/MeshExporter3mf_tests.cpp
Task: T032 Add quantization mode and palette override state in gladius/src/ui/MeshExportDialog.h
```

## Parallel Example: User Story 5

```text
Task: T036 Add planner tests for no-target standards-limited decisions in gladius/tests/unittests/io/3mf/ColorCompatibilityPlanner_tests.cpp
Task: T037 Add integration tests for explicit target export in gladius/tests/integrationtests/ColorExport_Integration_tests.cpp
```

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational.
3. Complete Phase 3: User Story 1.
4. Validate printable-region behavior with the User Story 1 independent test before expanding scope.

### Incremental Delivery

1. Deliver US1 to establish printable-region compatibility in the default export path.
2. Deliver US2 to improve fidelity while preserving deterministic compatibility.
3. Deliver US3 to lock in the non-shell default workflow guarantee.
4. Deliver US4 to expose compatibility tuning in the export dialog.
5. Deliver US5 to add explicit target-application fallback only where needed.
6. Finish with Phase 8 documentation and verification.

### Parallel Team Strategy

1. One developer can handle helper files and shared contracts in Phases 1-2.
2. After Foundational completion:
   - Developer A: US1 writer/exporter fallback path
   - Developer B: US2 quantization and warning logic
   - Developer C: US4 dialog controls and US5 target-application UI/plumbing
3. Rejoin for Phase 8 validation and documentation.

---

## Notes

- `[P]` marks tasks that touch different files and can be executed in parallel.
- `[US1]` through `[US5]` map directly to the user stories in `spec.md`.
- The MVP scope is **User Story 1 only**.
- Default exports must remain standards-only unless a user explicitly selects a target application.
- Shell export stays out of this feature’s default workflow.
