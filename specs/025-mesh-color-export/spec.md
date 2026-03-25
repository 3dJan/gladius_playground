# Feature Specification: Default Mesh Color Export

**Feature Branch**: `025-mesh-color-export`  
**Created**: 2026-03-25  
**Status**: Draft  
**Input**: User description: "Extend the 3MF mesh export so colors are exported by default using only standardized 3MF features. Exported colors must be readable by PrusaSlicer and Orca, preserve as much color detail as possible, prefer higher-fidelity standard representations when they are compatible, and keep shell export out of the default raw-color workflow."

## Clarifications

### Session 2026-03-25

- Q: What does “readable by PrusaSlicer and Orca” mean? → A: Colors must be interpretable as printable material/extruder regions, not just visual color.
- Q: What should happen if higher-fidelity standard 3MF color representations do not produce printable regions in both slicers? → A: Fall back to discrete standard 3MF regions/components/objects that remain assignable to materials/extruders.
- Q: How should color quantization behave when printable-region compatibility requires simplification? → A: Quantize automatically to an adaptive palette per export when needed, and make that behavior configurable in the export dialog.
- Q: What should happen if standard-only export still cannot preserve printable regions? → A: Keep the default export standard-only, and offer an optional target-application mode that adds proprietary slicer tags only when the user explicitly selects a target application.
- Q: Which color-representation order should the spec treat as canonical? → A: Texture → Vertex → Triangle → Component/Object → Build Item.
- Q: If standard-only export still cannot preserve printable regions and no target application is selected, what should happen? → A: Export the best standard-only result and show a warning that printable regions were not fully preserved.
- Q: How should transparency or unsupported alpha data be handled? → A: Ignore alpha for printable-region planning and export, and warn the user when transparency cannot be preserved.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Default export produces printable color regions in target slicers (Priority: P1)

As a user exporting a colored model from Gladius, I want the default 3MF mesh export to carry color information into PrusaSlicer and Orca as printable material or extruder regions so that the result can be used for multi-material printing rather than only visual inspection.

**Why this priority**: If the target slicers cannot interpret the exported color as printable regions, the feature fails its main purpose regardless of internal color fidelity.

**Independent Test**: Export a set of reference colored models using the default mesh 3MF export, import them into PrusaSlicer and Orca, and confirm that distinct printable regions are available for material or extruder assignment without any manual package editing or alternate export mode.

**Acceptance Scenarios**:

1. **Given** a model with color information, **When** the user performs the default 3MF mesh export, **Then** the exported file imports into PrusaSlicer with distinct printable regions that can be assigned to materials or extruders.
2. **Given** a model with color information, **When** the user performs the default 3MF mesh export, **Then** the exported file imports into Orca with distinct printable regions that can be assigned to materials or extruders.
3. **Given** a model without color information, **When** the user performs the default 3MF mesh export, **Then** the export still succeeds and remains usable as a normal uncolored mesh export.

---

### User Story 2 - Highest compatible printable detail is preserved (Priority: P2)

As a user working with detailed color gradients or textured appearance, I want the default export to preserve as much raw color detail as the target slicers can reliably preserve while still producing printable material or extruder regions so that imported models retain as much printable variation as possible, even when the export must fall back to discrete standard regions or objects.

**Why this priority**: Printable-region interpretation is more important than fidelity, but once the slicers can use the export for printing the next most valuable outcome is keeping as much of the original color detail as practical.

**Independent Test**: Export colored reference models that contain gradients, sharp boundaries, and repeated colors, then verify that the exporter chooses the highest-fidelity standard representation that still produces printable regions in both target slicers.

**Acceptance Scenarios**:

1. **Given** a colored model, **When** multiple standard color representations are available, **Then** the exporter uses the highest-fidelity representation that still yields printable regions in both target slicers.
2. **Given** a higher-fidelity representation is not reliably interpretable as printable regions in one or both target slicers, **When** export occurs, **Then** the exporter falls back automatically to a lower-fidelity standard representation that is, including discrete standard regions, components, or objects when necessary.
3. **Given** a model with repeated colors, **When** export occurs, **Then** equivalent colors are represented consistently so the imported result preserves printable regions without unnecessary variation.
4. **Given** printable-region compatibility requires simplification, **When** export occurs, **Then** the exporter may adaptively quantize colors for that export and exposes that behavior through the export dialog.

---

### User Story 3 - Default export remains a raw mesh color workflow (Priority: P3)

As a user choosing the default mesh export, I want raw color data exported directly on the mesh instead of converting the model into experimental shell-based output so that the default export stays simple, predictable, and aligned with standard 3MF mesh exchange.

**Why this priority**: Users need a dependable default export path for standard mesh exchange; shell export remains a separate experimental workflow with different intent.

**Independent Test**: Export a colored model through the default mesh path and verify that the output contains a direct colored mesh representation rather than a shell-derived approximation or material-stack decomposition.

**Acceptance Scenarios**:

1. **Given** shell export capabilities exist separately, **When** the user performs the default colored mesh export, **Then** the system exports the original mesh with raw color data rather than shell-generated geometry.
2. **Given** a user wants hueforge-style or thickness-based color effects, **When** they use export features, **Then** those effects remain in the separate shell export workflow and are not automatically substituted into the default mesh export.
3. **Given** a high-fidelity raw-color representation is incompatible with printable-region interpretation in one or both target slicers, **When** the default export occurs, **Then** the exporter may fall back to discrete standard regions, components, or objects without invoking shell export.

---

### User Story 4 - Export dialog allows compatibility tuning (Priority: P3)

As a user exporting a colored model, I want the export dialog to expose the compatibility-driven color simplification behavior so that I can tune or inspect how aggressively the exporter reduces color detail to preserve printable regions.

**Why this priority**: The default behavior must work automatically, but users need a clear and controllable escape hatch when compatibility-driven quantization changes the output more than they want.

**Independent Test**: Open the export dialog for a colored model, confirm that quantization behavior is configurable there, and verify that changing the setting affects the exported printable-region breakdown.

**Acceptance Scenarios**:

1. **Given** a colored model export, **When** the export dialog is shown, **Then** the dialog includes configuration for the compatibility-driven quantization behavior.
2. **Given** the user changes the quantization setting in the export dialog, **When** export occurs, **Then** the exported printable-region result reflects that setting.

---

### User Story 5 - Optional target-application mode handles non-standard cases (Priority: P3)

As a user exporting a colored model, I want the export dialog to offer an optional target-application mode so that I can explicitly optimize for a specific slicer with proprietary tags only when standard 3MF export cannot preserve printable regions.

**Why this priority**: The default workflow should remain portable and standards-based, but users still need a controlled fallback when standard-only export cannot meet the printable-region requirement.

**Independent Test**: Attempt a colored export for a model/settings combination where standard-only export cannot preserve printable regions, then select a target application in the export dialog and verify that the resulting file is optimized for that slicer without manual package editing.

**Acceptance Scenarios**:

1. **Given** standard-only export is sufficient, **When** the user performs the default export, **Then** the file contains no proprietary slicer-specific tags.
2. **Given** standard-only export cannot preserve printable regions for a model/settings combination, **When** the user explicitly selects a target application, **Then** the exporter may add proprietary tags for that target application.
3. **Given** the user does not select a target application, **When** standard-only export cannot preserve printable regions, **Then** the exporter completes using the best standard-only result, does not add proprietary tags, and warns that printable regions were not fully preserved.

### Edge Cases

- What happens when a model contains color information but the highest-fidelity standard representation is visible in a target slicer without producing printable material or extruder regions?
- What happens when preserving printable-region interpretation requires the exporter to split a single colored mesh into multiple standard regions, components, or objects?
- What happens when adaptive quantization would need to collapse very fine gradients into a small number of printable regions?
- What happens when the user selects one target application and opens the result in a different slicer?
- What happens when a model mixes large flat color regions with high-detail gradients in the same export?
- What happens when the mesh export contains multiple components or build items with different color usage patterns?
- If the input contains transparency or unsupported alpha data, the exporter ignores alpha for printable-region planning and warns that transparency was not preserved.
- What happens when color data is missing, partially defined, or only available on some parts of the model?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The default 3MF mesh export MUST include color information automatically whenever the exported model contains color data.
- **FR-002**: The default colored mesh export MUST remain standards-based and MUST use only standardized 3MF color and material representation mechanisms unless the user explicitly selects a target-application mode.
- **FR-003**: The exporter MUST first attempt to preserve printable material or extruder regions in both PrusaSlicer and Orca using only standardized 3MF mechanisms.
- **FR-004**: The exporter MUST preserve the original mesh export workflow and MUST NOT replace the default colored mesh export with shell-based or thickness-derived geometry.
- **FR-005**: The exporter MUST choose the highest-fidelity standard color representation that is confirmed to produce printable material or extruder regions in both target slicers.
- **FR-006**: When the most detailed standard representation does not produce printable material or extruder regions in one or both target slicers, the exporter MUST fall back automatically to the next lower-fidelity standard representation that does.
- **FR-006a**: When necessary to preserve printable material or extruder assignment in both target slicers, the fallback strategy MUST permit conversion from a single high-fidelity colored mesh into discrete standard 3MF regions, components, or objects that remain assignable to materials or extruders.
- **FR-006b**: When standard-only export still cannot preserve printable material or extruder regions, the exporter MUST allow the user to explicitly select a target application mode that may add proprietary slicer-specific tags for that selected application.
- **FR-006c**: The exporter MUST NOT add proprietary slicer-specific tags unless the user explicitly selects a target application mode.
- **FR-006d**: When a target application mode is selected, the exporter MUST restrict proprietary tagging to the selected application rather than mixing multiple slicer-specific proprietary formats in the same export.
- **FR-006e**: When standard-only export cannot preserve printable material or extruder regions and no target application is selected, the exporter MUST still produce the best available standard-only result and MUST warn that printable-region fidelity was not fully preserved.
- **FR-007**: The canonical representation preference order MUST be texture-backed color, then vertex-interpolated color, then triangle color, then discrete component/object color partitioning, and finally build-item-level fallback, provided each option remains readable in both target slicers.
- **FR-008**: The exporter MUST preserve raw source color intent as closely as possible within the chosen printable representation, including gradients, discrete boundaries, and repeated palette colors.
- **FR-009**: The exporter MUST continue to produce a valid and usable 3MF mesh export when a model contains no color data.
- **FR-010**: The exporter MUST handle models containing multiple mesh parts or components without losing color assignments during export, including cases where fallback introduces additional standard regions, components, or objects.
- **FR-011**: The exporter MUST apply a deterministic fallback strategy so the same input model produces the same color export representation under the same compatibility profile.
- **FR-012**: The exporter MUST surface when color detail had to be reduced because a higher-fidelity standard representation was incompatible with one or both target slicers.
- **FR-013**: When printable-region compatibility requires simplification, the exporter MUST support adaptive per-export color quantization.
- **FR-014**: The export dialog MUST expose configuration for the quantization behavior used by the default colored mesh export.
- **FR-015**: Under the same input model and export dialog settings, adaptive quantization MUST behave deterministically.
- **FR-016**: The export dialog MUST expose the target application selection for optional proprietary export optimization.
- **FR-017**: When the user selects a target application mode, the exporter MUST clearly indicate that portability to other slicers may be reduced.
- **FR-018**: When the input contains transparency or unsupported alpha data, the exporter MUST ignore alpha for printable-region planning and MUST warn that transparency was not preserved in the exported result.

### Key Entities *(include if feature involves data)*

- **Source Color Data**: The original color information associated with the model being exported, including raw surface colors, gradients, repeated palette values, and per-region color variation.
- **Color Export Representation**: The standardized 3MF mechanism selected for the export, including its fidelity level, compatibility status, and fallback position.
- **Discrete Printable Region**: A standard 3MF region, component, or object introduced during fallback so a target slicer can preserve assignable material or extruder boundaries.
- **Compatibility Profile**: The set of constraints the default export must satisfy so PrusaSlicer and Orca interpret the result as printable material or extruder regions.
- **Colored Mesh Export Result**: The final 3MF output produced by the default mesh export path, including mesh geometry, color assignments, and any recorded fallback decisions.
- **Quantization Setting**: The export-dialog configuration that controls how the exporter reduces color detail when needed to preserve printable-region compatibility.
- **Target Application Selection**: The export-dialog choice that keeps the export standard-only by default or explicitly optimizes for a selected slicer with proprietary tags.

### Assumptions

- PrusaSlicer and Orca compatibility will be evaluated against currently supported stable versions at implementation time.
- If a higher-fidelity standard representation is not reliably interpreted as printable material or extruder regions in both target slicers, printable-region interpretation takes priority over maximum theoretical fidelity.
- The fallback path may restructure the exported model into discrete standard 3MF regions, components, or objects, but must remain within the default mesh export workflow and must not invoke shell export.
- Adaptive quantization is part of the default export workflow, but users can configure its behavior through the export dialog.
- Optional proprietary tagging is only used when the user explicitly selects a target application in the export dialog.
- Transparency is treated as non-printable appearance data for this feature and is not preserved as a printable-region control signal.
- The shell export workflow remains available as a separate experimental feature and is out of scope for the default raw-color mesh export.
- The feature applies to mesh-based 3MF export only and does not redefine how implicit or volumetric data is exchanged internally.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of approved standard-capable colored reference models exported through the default mesh 3MF workflow import into both PrusaSlicer and Orca with printable material or extruder regions available for assignment.
- **SC-002**: For approved reference models containing at least 8 distinct source color regions, at least 90% of those regions remain separable as distinct printable regions after import into both target slicers.
- **SC-003**: For approved gradient reference models, the exported result uses the highest-fidelity standard representation that is verified to preserve printable-region interpretation in both target slicers, falling back to discrete standard regions, components, or objects when necessary, with no manual post-processing required by the user.
- **SC-004**: 100% of approved uncolored reference models continue to export successfully through the default mesh 3MF workflow without requiring the user to opt out of color export.
- **SC-005**: Users can complete a default colored mesh export using the same primary export workflow as today, without being forced into the shell export path.
- **SC-006**: Users can identify and modify the quantization behavior for colored mesh export from the export dialog without leaving the standard mesh export workflow.
- **SC-007**: For approved fallback reference models where standard-only export cannot preserve printable regions, users can select a target application in the export dialog and produce a file that preserves printable regions in that selected slicer without manual package editing.
