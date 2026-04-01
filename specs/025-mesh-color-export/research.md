# Research: Default Mesh Color Export

**Feature**: 025-mesh-color-export  
**Date**: 2026-03-25

## Decision 1: Keep the default workflow on the existing mesh export path

**Decision**: Implement the feature on the existing mesh 3MF export path centered on `gladius/src/io/MeshExporter3mf.cpp`, `gladius/src/io/3mf/MeshWriter3mf.cpp`, and `gladius/src/ui/MeshExportDialog.cpp`, not on the shell export pipeline.

**Rationale**:
- The spec explicitly keeps shell export out of the default raw-color workflow.
- `MeshExporter3mf` already performs volumetric color sampling and dispatches to `MeshWriter3mf`.
- `MeshWriter3mf` already supports standard 3MF color export via `exportMeshWithColors()` and `exportMeshWithVertexColors()`.
- The main UI already exposes color export toggles in `MeshExportDialog.cpp`, so extending that dialog is lower churn than introducing a parallel export entry point.

**Alternatives considered**:
- **Reuse shell export**: Rejected because shell export intentionally produces thickness-driven geometry rather than raw colored meshes.
- **Build a new standalone exporter**: Rejected because current exporter/writer/dialog classes already cover most of the pipeline and tests.

## Decision 2: Use a standards-first compatibility ladder with triangle and discrete fallback

**Decision**: The exporter will attempt standardized 3MF representations in the canonical order `Texture → Vertex → Triangle → Component/Object → Build Item`, then fall back deterministically through the remaining standard options when higher-fidelity representations do not preserve printable regions in PrusaSlicer and Orca.

**Rationale**:
- The feature requires standard-only output by default.
- Existing code already supports per-face and per-vertex color groups through lib3mf.
- Triangle color needs to be explicit in the ladder because it sits between interpolated appearance and discrete partitioning in both fidelity and implementation cost.
- Prior slicer research shows that visual color support and printable-region support are not identical; printable-region compatibility may require reducing the representation to discrete assignable regions.
- Discrete standard components/objects and, at the lowest fidelity, build-item-level separation are the safest standard-only fallbacks for material/extruder assignment semantics.

**Alternatives considered**:
- **Per-vertex color only**: Rejected because it maximizes fidelity but is not reliable for printable-region interpretation.
- **Per-face/triangle color only**: Rejected as a complete strategy because it may still be interpreted as appearance rather than assignable regions.
- **Texture-first implementation only**: Rejected because texture-backed color may preserve detail but still fail the printable-region requirement in target slicers.

## Decision 3: Quantize adaptively per export, and expose it in the main export dialog

**Decision**: Add adaptive per-export color quantization, enabled when compatibility requires discrete printable regions, and make the behavior configurable in the main `MeshExportDialog`.

**Rationale**:
- The spec requires preserving as much detail as possible while still producing printable regions.
- A fixed global palette is too blunt for models with different complexity.
- The UI already exposes `Export with colors`, `Convert to sRGB`, and `Color Mode`; adaptive quantization belongs beside those controls.
- Deterministic adaptive quantization can satisfy both user control and reproducibility requirements.

**Alternatives considered**:
- **No automatic quantization**: Rejected because some models would never reach printable-region compatibility.
- **Fixed palette for all exports**: Rejected because it wastes detail on simple models and remains insufficiently flexible on complex ones.
- **Hidden quantization with no UI**: Rejected because the spec requires configurability and user visibility.

## Decision 4: Add optional target-application proprietary tagging only by explicit user choice

**Decision**: Keep the default export standards-based; if standard-only export still cannot preserve printable regions, allow the user to choose a target application mode (`None`, `PrusaSlicer`, `Orca`) that enables proprietary slicer-specific tagging.

**Rationale**:
- The feature’s default behavior must remain portable.
- The spec explicitly allows proprietary tags only as an opt-in fallback.
- `Writer3mfBase` already centralizes metadata copying and default metadata injection, making it the natural place to add optional target-specific metadata/tagging helpers.
- Restricting proprietary tagging to one selected application avoids mixed-format ambiguity.

**Alternatives considered**:
- **Automatically add proprietary tags when standard export fails**: Rejected because it would silently reduce portability.
- **Always tag for the chosen target even when standard export works**: Rejected because it needlessly abandons portability.
- **Fail hard when standard export cannot satisfy the requirement**: Rejected because the clarified spec prefers an explicit target-application fallback over hard failure.

## Decision 5: When no target application is selected, export the best standard-only result and warn

**Decision**: If standard-only export still cannot preserve printable regions and the user has not selected a target application, the exporter completes with the best available standard-only representation and emits a warning that printable-region fidelity was not fully preserved.

**Rationale**:
- The spec keeps standards-only behavior as the default and forbids silently switching to proprietary tagging.
- Exporting a deterministic standards-limited result is less disruptive than blocking the workflow or silently dropping all colors.
- The warning gives the user a clear cue to retry with a target application only when they actually need slicer-specific optimization.

**Alternatives considered**:
- **Block export**: Rejected because it turns a portability preference into a hard stop.
- **Force a prompt mid-export**: Rejected because it complicates async export flow and weakens repeatability.
- **Drop to uncolored mesh**: Rejected because it discards useful standard color information unnecessarily.

## Decision 6: Ignore transparency/alpha for printable-region planning and warn when it is dropped

**Decision**: Treat transparency and unsupported alpha channels as non-printable appearance data. Ignore alpha during printable-region planning/export decisions and warn the user when transparency cannot be preserved.

**Rationale**:
- PrusaSlicer-family printable-region workflows focus on assignable material regions, not transparent surface rendering semantics.
- Using alpha as a printable control signal would be ambiguous and likely non-portable.
- Warning on dropped transparency preserves user awareness without making alpha handling a blocker for the core feature.

**Alternatives considered**:
- **Fail export when alpha is present**: Rejected because it is too strict for a secondary appearance channel.
- **Quantize alpha into material regions**: Rejected because it invents semantics that slicers do not interpret consistently.
- **Silently drop alpha**: Rejected because it hides a potentially meaningful appearance loss.

## Decision 7: Extend the current automated test suite and treat real slicer compatibility as a versioned acceptance matrix

**Decision**: Extend existing unit and integration tests for colored mesh export, and define a versioned compatibility matrix for PrusaSlicer and Orca as part of acceptance validation.

**Rationale**:
- `MeshWriter3mfColor_tests.cpp` and `ColorExport_Integration_tests.cpp` already validate standard color export paths.
- New behavior adds quantization, regionization, exporter settings, and optional target-specific tagging; these are straightforward to unit/integration test in-repo.
- Actual slicer behavior is an external compatibility concern and should be captured as a validated matrix against named slicer versions rather than assumed from lib3mf round-trips alone.

**Alternatives considered**:
- **Rely only on lib3mf round-trip tests**: Rejected because lib3mf validity does not prove slicer behavior.
- **Make slicer GUI automation part of CI**: Rejected for Phase 1 because it adds heavy tooling and OS-specific fragility; versioned manual/acceptance verification is sufficient for planning.

## Implementation Touchpoints Identified

- `gladius/src/io/MeshExporter3mf.cpp/.h` — current mesh export orchestration, color sampling, and writer dispatch
- `gladius/src/io/3mf/MeshWriter3mf.cpp/.h` — standard 3MF mesh writing, color groups, triangle properties
- `gladius/src/io/3mf/Writer3mfBase.cpp/.h` — metadata copying/default metadata, natural hook for target-application tagging
- `gladius/src/ui/MeshExportDialog.cpp/.h` — main export dialog already containing color export controls
- `gladius/tests/unittests/MeshExporter3mf_tests.cpp` — good home for settings snapshot, warning, and no-target fallback tests
- `gladius/tests/unittests/MeshWriter3mfColor_tests.cpp` — current color export unit coverage
- `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp` — current end-to-end color export coverage

## Constraints Confirmed

- Default export must remain standards-based.
- Shell export remains separate and must not be used as the fallback for this feature.
- Export must remain deterministic under the same settings.
- If standard-only export is insufficient and no target application is selected, the exporter must return the best standard-only result with a warning.
- Transparency/alpha is not part of printable-region planning and must not silently influence fallback decisions.
- UI must stay responsive; export processing must remain on the async exporter path.
