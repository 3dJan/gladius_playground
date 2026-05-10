# Implementation Plan: Default Mesh Color Export

**Branch**: `025-mesh-color-export` | **Date**: 2026-03-25 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/025-mesh-color-export/spec.md`

## Summary

Extend the existing mesh 3MF export pipeline so color is exported by default through a standards-first compatibility ladder. The implementation stays on the current `MeshExportDialog` → `MeshExporter3mf` → `MeshWriter3mf` path, evaluates the canonical order `Texture → Vertex → Triangle → Component/Object → Build Item`, adaptively quantizes and regionizes when needed for printable-region compatibility, emits a standards-limited warning when no target application is selected, ignores transparency for printable-region planning with an explicit warning, and only enables proprietary slicer tagging when the user explicitly opts into a target application.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: lib3mf, OpenCL 1.2+, Eigen3, ImGui, fmt, STL  
**Storage**: 3MF package files on disk; in-memory mesh/color buffers during export  
**Testing**: GTest/GMock unit tests, integration tests, versioned manual slicer compatibility matrix  
**Target Platform**: Linux desktop (primary), Windows desktop (secondary); PrusaSlicer and Orca as target consumers  
**Project Type**: Single C++ desktop application with GPU-assisted mesh extraction and ImGui UI  
**Performance Goals**: Preserve existing async export UX; keep UI responsive at all times; keep compatibility-planning overhead subdominant to mesh generation/export cost for 100k-triangle reference models; surface progress/warnings without blocking the UI thread  
**Constraints**: Standards-only by default, no shell export fallback, deterministic quantization/regionization, proprietary tags only on explicit target selection, no silent portability loss, transparency ignored for printable-region planning  
**Scale/Scope**: One main export dialog, one mesh export orchestration layer, one 3MF writer layer, a small set of new helper classes, and extensions to existing unit/integration tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | New helpers can remain focused C++20 value/service types using STL and exceptions for invalid export configurations |
| II. Test-First Development | ✅ Pass | New planner/quantizer/warning behavior requires unit and integration coverage before implementation is complete |
| III. Simplicity First | ✅ Pass | Extends current exporter/writer/dialog seams instead of adding a parallel export subsystem |
| IV. Consistent Code Style | ✅ Pass | Work stays in existing C++/ImGui/lib3mf code paths and existing Gladius naming/layout conventions |
| V. Documentation and Comments | ✅ Pass | Contracts, quickstart, and Doxygen updates cover public exporter/settings additions |
| VI. UI Responsiveness | ✅ Pass | Export work remains asynchronous; warnings/progress must be surfaced without blocking the UI thread |

## Project Structure

### Documentation (this feature)

```text
specs/025-mesh-color-export/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── MeshColorExportSettings.api.md
│   └── CompatibilityDecision.api.md
└── tasks.md
```

### Source Code (repository root)

```text
gladius/src/
├── io/
│   ├── MeshExporter3mf.cpp/.h                # MODIFY: capture settings, run compatibility planner, orchestrate fallback/warnings
│   └── 3mf/
│       ├── MeshWriter3mf.cpp/.h              # MODIFY: add triangle/discrete/build-item standard write paths
│       ├── Writer3mfBase.cpp/.h              # MODIFY: optional target-application metadata/tagging hooks
│       ├── FaceColors.h                      # REUSE: color data types
│       ├── FaceColorSampler.cpp/.h           # REUSE: volumetric color sampling
│       ├── ColorCompatibilityPlanner.cpp/.h  # NEW: standards-first decision engine
│       ├── ColorQuantizer.cpp/.h             # NEW: deterministic adaptive quantization
│       └── ColorRegionizer.cpp/.h            # NEW: discrete printable-region decomposition
├── ui/
│   └── MeshExportDialog.cpp/.h               # MODIFY: add quantization + target application controls and warnings
└── compute/
    └── ProgramManager / sampling path        # REUSE: no new compute pipeline expected for Phase 1

gladius/tests/
├── unittests/
│   ├── MeshWriter3mfColor_tests.cpp          # MODIFY: standard triangle/discrete/build-item fallback coverage
│   ├── MeshExporter3mf_tests.cpp             # NEW: settings snapshot and warning behavior
│   ├── ColorQuantizer_tests.cpp              # NEW
│   └── ColorCompatibilityPlanner_tests.cpp   # NEW
└── integrationtests/
    └── ColorExport_Integration_tests.cpp     # MODIFY: standards-only fallback, no-target warning, alpha warning, target-app scenarios
```

**Structure Decision**: Keep the feature inside the existing mesh export architecture. The main extension points are `MeshExportDialog.cpp` for UI, `MeshExporter3mf` for orchestration, `MeshWriter3mf` for standards-based 3MF output, `Writer3mfBase` for optional target-specific metadata/tagging, and dedicated helper classes for planner/quantizer/regionization logic.

## Implementation Phases

### Phase 0: Research ✅

See [research.md](research.md) for the decision log.

**Outcome**:
- Default workflow remains standards-first and stays on the existing mesh export path
- Canonical representation order is `Texture → Vertex → Triangle → Component/Object → Build Item`
- Standard discrete region/component/object fallback is preferred over shell export
- If no target application is selected, standards-limited export completes with a warning rather than silently switching modes
- Transparency/alpha is ignored for printable-region planning and produces an explicit warning
- Proprietary target tagging remains optional and explicit only

### Phase 1: Design

#### Data Model

See [data-model.md](data-model.md).

Key design entities:
- `MeshColorExportSettings`
- `CompatibilityProfile`
- `CompatibilityDecision`
- `QuantizedPalette`
- `PrintableRegion`
- `ColoredMeshExportResult`

#### Workflow Changes

```text
BEFORE
  MeshExportDialog
        │
        ▼
  MeshExporter3mf
        │
        ├── no colors       ──▶ MeshWriter3mf::exportMesh
        └── face/vertex color ─▶ MeshWriter3mf::exportMeshWithColors / exportMeshWithVertexColors

AFTER
  MeshExportDialog
    ├── export with colors
    ├── convert to sRGB
    ├── color mode
    ├── quantization mode
    └── target application
        │
        ▼
  MeshExporter3mf
    ├── sample source colors
    ├── build compatibility profile
    ├── choose standards-first representation ladder
    ├── quantize/regionize if needed
    ├── ignore alpha for printable planning and emit warning if needed
    ├── return standards-limited warning when no target app is selected
    └── optionally enable target-specific tagging
        │
        ▼
  MeshWriter3mf / Writer3mfBase
    ├── standard texture/vertex/triangle path
    ├── standard discrete components/objects path
    ├── standard build-item fallback path
    └── optional proprietary target-specific metadata/tag path
```

#### Contracts

See:
- [contracts/MeshColorExportSettings.api.md](contracts/MeshColorExportSettings.api.md)
- [contracts/CompatibilityDecision.api.md](contracts/CompatibilityDecision.api.md)

### Phase 2: Implementation Planning

| # | Task | Effort | Priority | Depends |
|---|------|--------|----------|---------|
| 1 | Extend `MeshExportDialog` with quantization, target app, and warning UI | 2h | P1 | - |
| 2 | Extend `MeshExporter3mf` settings API, immutable export snapshot, and warning plumbing | 2h | P1 | 1 |
| 3 | Implement deterministic `ColorQuantizer` | 3h | P1 | 2 |
| 4 | Implement `ColorCompatibilityPlanner` with canonical representation order | 3h | P1 | 2 |
| 5 | Implement `ColorRegionizer` and standard component/object fallback | 4h | P1 | 3, 4 |
| 6 | Extend `MeshWriter3mf` for triangle/discrete/build-item standard write paths | 4h | P1 | 5 |
| 7 | Extend `Writer3mfBase` for optional target application tagging and warning metadata hooks | 2h | P2 | 4 |
| 8 | Add/update unit tests, including `MeshExporter3mf_tests.cpp` | 3h | P1 | 3, 4, 6 |
| 9 | Add/update integration tests for no-target warning, alpha warning, and multipart fallback | 3h | P1 | 6, 7 |
| 10 | Validate responsiveness/progress reporting on async export path | 2h | P2 | 2, 9 |
| 11 | Validate against supported PrusaSlicer/Orca versions and record matrix results | 2h | P2 | 9 |

**Total Estimate**: ~30h

## Risk Assessment

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Standard texture/vertex/triangle paths do not satisfy printable-region semantics | High | High | Make discrete components/objects and build-item separation the deterministic standard fallback path |
| Canonical representation order drifts again across artifacts | High | Medium | Keep spec, contracts, plan, and future task refresh aligned on the same enum/order |
| Quantization becomes nondeterministic | High | Medium | Use stable ordering, deterministic clustering, and explicit repeatability tests |
| Proprietary target tagging leaks into default export | High | Low | Gate all target-specific behavior on explicit `TargetApplication != None` |
| Alpha/transparency expectations confuse users | Medium | Medium | Emit explicit warning when transparency is ignored and document the behavior in contracts/quickstart |
| UI regressions from planner/warning work | High | Low | Keep planning work on async export path and validate responsiveness/progress behavior |

## Complexity Tracking

> No constitution violations are currently anticipated.

| Aspect | Complexity | Justification |
|--------|------------|---------------|
| Compatibility planner | Moderate | Centralizes policy/fallback logic instead of scattering conditionals |
| Quantizer + regionizer + build-item fallback | Moderate | Necessary to bridge raw color fidelity and printable-region compatibility |
| Optional proprietary mode | Moderate | Isolated fallback path, explicit user opt-in keeps default path simple |

## Post-Design Constitution Check

*GATE: Re-evaluation after Phase 1 design completion.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | New helpers remain focused value types/services using STL and exceptions |
| II. Test-First Development | ✅ Pass | Unit tests for planner/quantizer/exporter warnings and integration tests for fallback modes are explicitly planned |
| III. Simplicity First | ✅ Pass | Design extends existing exporter/writer/dialog path rather than introducing a second export subsystem |
| IV. Consistent Code Style | ✅ Pass | Files fit existing naming, layout, and layering conventions |
| V. Documentation and Comments | ✅ Pass | Contracts, quickstart, and future Doxygen updates cover public API additions |
| VI. UI Responsiveness | ✅ Pass | Planning/quantization stays in async export flow; warning/progress surfacing is explicitly planned |

**Design Validation**: No constitution violations identified. The design preserves the current export architecture, keeps the default path standards-based, makes standards-limited fallback explicit, and isolates proprietary behavior behind explicit user choice.
