# Quickstart: Default Mesh Color Export

**Feature**: 025-mesh-color-export  
**Date**: 2026-03-25

## Overview

This guide describes how to implement the standards-first colored mesh export workflow, keeping the existing mesh export pipeline intact while adding compatibility planning, adaptive quantization, a canonical `Texture → Vertex → Triangle → Component/Object → Build Item` fallback ladder, standards-limited warning behavior, and optional target-application proprietary tagging.

## Architecture Summary

```text
┌──────────────────────────────────────────────────────────────────────────┐
│                Main Mesh Export Dialog (existing entry point)            │
└──────────────────────────────────────────────────────────────────────────┘
                │
                ▼
      ┌───────────────────────┐
      │ MeshExportDialog.cpp  │
      │ - Export with colors  │
      │ - Color mode          │
      │ - Quantization mode   │
      │ - Target application  │
      └──────────┬────────────┘
                 │
                 ▼
      ┌───────────────────────┐
      │ MeshExporter3mf       │
      │ - sample colors       │
      │ - build settings      │
      │ - run compatibility   │
      │   planner             │
      │ - emit warnings       │
      └──────────┬────────────┘
                 │
        ┌────────┴─────────────────────────────┐
        ▼                                      ▼
┌───────────────────────┐            ┌─────────────────────────┐
│ Standard path         │            │ Standard discrete path  │
│ texture/vertex/triangle │           │ quantize + regionize    │
└──────────┬────────────┘            └──────────┬──────────────┘
           │                                     │
           └──────────────────┬──────────────────┘
                              ▼
                     ┌───────────────────────┐
                     │ MeshWriter3mf         │
                     │ Writer3mfBase         │
                     └──────────┬────────────┘
                                │
              ┌─────────────────┴──────────────────┐
              ▼                                    ▼
     standard-only write                optional target app tags
```

## Step 1: Extend export settings in the main dialog

**Files**:
- `gladius/src/ui/MeshExportDialog.h`
- `gladius/src/ui/MeshExportDialog.cpp`

Add new export-state fields near the existing color settings:

```cpp
bool m_exportWithColors = true;
bool m_convertToSrgb = true;
io::ColorMode m_colorMode = io::ColorMode::PerFace;
QuantizationMode m_quantizationMode = QuantizationMode::Adaptive;
TargetApplication m_targetApplication = TargetApplication::None;
std::int32_t m_maxPaletteSize = 0; // 0 = automatic
```

Add new ImGui controls directly below the existing color export controls:

- quantization mode selector
- optional max palette size override
- target application selector
- warning text when target application is not `None`

Keep all controls inside the existing 3MF color export section instead of creating a parallel dialog.

## Step 2: Capture settings in `MeshExporter3mf`

**Files**:
- `gladius/src/io/MeshExporter3mf.h`
- `gladius/src/io/MeshExporter3mf.cpp`

Add additive setter methods:

```cpp
void setQuantizationMode(QuantizationMode mode);
void setMaxPaletteSize(std::optional<std::uint32_t> maxPaletteSize);
void setTargetApplication(TargetApplication targetApplication);
```

In `finalize()`:
1. convert the grid to mesh
2. sample source colors (existing code)
3. build a `MeshColorExportSettings` snapshot
4. run a compatibility planner
5. dispatch either:
   - direct standard write, or
   - quantized/regionized standard write, or
  - build-item-level standard write, or
   - optional target-specific tag path

## Step 3: Add a deterministic compatibility planner

**New files**:
- `gladius/src/io/3mf/ColorCompatibilityPlanner.h`
- `gladius/src/io/3mf/ColorCompatibilityPlanner.cpp`
- `gladius/src/io/3mf/ColorQuantizer.h`
- `gladius/src/io/3mf/ColorQuantizer.cpp`

Responsibilities:
- determine the compatibility profile for `None`, `PrusaSlicer`, `Orca`
- evaluate the canonical standards-first representation ladder
- decide whether quantization is required
- decide whether regionization is required
- decide whether optional proprietary tagging is allowed
- emit deterministic warnings and a final `CompatibilityDecision`
- ignore alpha for printable-region planning while preserving a user-visible warning when transparency is dropped

Determinism requirements:
- stable color ordering
- stable region ordering
- no unseeded random clustering
- identical inputs + settings produce identical palette assignments

## Step 4: Add discrete-region standard fallback

**Likely files**:
- `gladius/src/io/3mf/MeshWriter3mf.h`
- `gladius/src/io/3mf/MeshWriter3mf.cpp`
- optional new helper: `gladius/src/io/3mf/ColorRegionizer.h/.cpp`

Add an additive write path that can emit:
- per-component standard regions, or
- multiple mesh objects/build items derived from palette regions

This path should:
- accept a quantized palette
- accept region membership per triangle
- remain standards-only
- avoid shell export entirely
- support a final build-item-only fallback before any proprietary path is considered

## Step 5: Add optional target-application metadata/tagging

**Files**:
- `gladius/src/io/3mf/Writer3mfBase.h`
- `gladius/src/io/3mf/Writer3mfBase.cpp`
- optional target-specific helper files if package edits exceed metadata-only operations

Add a helper like:

```cpp
void addTargetApplicationMetadata(
    Lib3MF::PModel model3mf,
    TargetApplication targetApplication,
    CompatibilityDecision const& decision);
```

Rules:
- only execute when user selected a target application
- never mix PrusaSlicer and Orca proprietary encodings in one export
- keep the default path untouched when `targetApplication == None`
- if package-level edits beyond lib3mf are required, perform them only in this explicit target-specific path

## Step 6: Extend tests

**Unit tests**:
- `gladius/tests/unittests/MeshWriter3mfColor_tests.cpp` — extend with regionized export coverage
- new `gladius/tests/unittests/ColorQuantizer_tests.cpp`
- new `gladius/tests/unittests/ColorCompatibilityPlanner_tests.cpp`

**Integration tests**:
- `gladius/tests/integrationtests/ColorExport_Integration_tests.cpp`
  - standard-only compatible model
  - adaptive quantization fallback
  - no-target standards-limited warning path
  - target-application selection path
  - transparency/alpha warning path
  - no-color model still exports normally

**Acceptance matrix**:
- keep a small named corpus of reference models
- validate against supported stable versions of PrusaSlicer and Orca during feature verification

## Suggested Implementation Order

1. Add dialog settings and exporter setters
2. Add quantizer + planner with unit tests
3. Wire planner into `MeshExporter3mf::finalize()`
4. Add standard discrete-region writer path
5. Add standards-limited warning path and build-item fallback
6. Add optional target-application tagging path
7. Extend integration tests and acceptance matrix docs

## Risks to Watch

- Current color controls live in `MeshExportDialog.cpp`, not `MeshExportDialog3mf.cpp`; extending the wrong UI seam will create dead settings.
- `MeshWriter3mf` currently writes face/vertex colors, but not discrete object/component fallback.
- Build-item fallback needs to remain explicitly standards-only and must not be mistaken for proprietary tagging.
- lib3mf validity alone does not prove slicer behavior; compatibility rules must be empirical and versioned.
- Optional target-specific tagging must be kept sharply isolated from the default standards-based path.
- Alpha/transparency must not silently alter printable-region planning.

## Non-Shell Default Workflow Guarantee

The default colored mesh export pipeline operates entirely within `MeshExporter3mf::finalize()` and `MeshWriter3mf`. It **never** routes through `ShellExporter` or any shell-based geometry pipeline. This is by design:

- `MeshExporter3mf` samples face colors via GPU, runs the compatibility planner, and dispatches to `MeshWriter3mf` without involving shell generation.
- Shell-based export (`ShellExporter`) is a separate workflow enabled only by the explicit "Export shells with LUT" checkbox in the dialog.
- The compatibility planner's standard representations (Texture, Vertex, Triangle, Components, Objects, Build Items) all produce direct mesh output, not shell-derived geometry.
- Integration tests explicitly verify that the default color path does not invoke the shell export pipeline.
