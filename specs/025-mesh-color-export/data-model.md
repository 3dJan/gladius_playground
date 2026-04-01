# Data Model: Default Mesh Color Export

**Feature**: 025-mesh-color-export  
**Date**: 2026-03-25

## Overview

This feature extends the mesh-based 3MF export pipeline with a standards-first compatibility planner. The planner selects the highest-fidelity representation that still preserves printable regions in target slicers, then records any quantization, regionization, or target-specific fallback applied during export.

## Entity Relationship Diagram

```text
┌──────────────────────┐
│ Source Color Data    │
│----------------------│
│ raw colors           │
│ gradients            │
│ repeated regions     │
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│ MeshColorExportSettings │
│----------------------│
│ exportWithColors     │
│ convertToSrgb        │
│ preferredMode        │
│ quantizationMode     │
│ targetApplication    │
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐       ┌──────────────────────┐
│ CompatibilityProfile │──────▶│ CompatibilityDecision │
│----------------------│       │----------------------│
│ supportsTexture      │       │ finalRepresentation  │
│ supportsVertexColor  │       │ needsQuantization    │
│ supportsDiscreteObjs │       │ needsRegionization   │
│ allowsProprietary    │       │ warnings[]           │
└──────────┬───────────┘       └──────────┬───────────┘
           │                               │
           ▼                               ▼
┌──────────────────────┐       ┌──────────────────────┐
│ QuantizedPalette     │──────▶│ PrintableRegionSet   │
│----------------------│       │----------------------│
│ colors[]             │       │ regions[]            │
│ sourceToPaletteMap   │       │ triangle membership  │
│ maxError             │       │ assigned palette id  │
└──────────┬───────────┘       └──────────┬───────────┘
           │                               │
           └──────────────┬────────────────┘
                          ▼
                ┌──────────────────────┐
                │ ColoredMeshExportResult │
                │----------------------│
                │ mesh/object layout   │
                │ metadata/tags        │
                │ fallback report      │
                └──────────────────────┘
```

## Core Entities

### MeshColorExportSettings

User-controlled settings captured from the main export dialog.

```cpp
struct MeshColorExportSettings
{
    bool exportWithColors = true;
    bool convertToSrgb = true;
    ColorMode preferredColorMode = ColorMode::PerFace;
    QuantizationMode quantizationMode = QuantizationMode::Adaptive;
    TargetApplication targetApplication = TargetApplication::None;
    std::optional<std::uint32_t> maxPaletteSize;
};
```

**Validation rules**:
- `targetApplication == None` implies standards-only export
- `maxPaletteSize`, when set, must be >= 2
- `quantizationMode == Adaptive` may only affect output when compatibility fallback is needed

### CompatibilityProfile

An internal rule set describing what export representations are considered usable for a given compatibility target.

```cpp
struct CompatibilityProfile
{
    bool requiresPrintableRegions = true;
    bool supportsTextureForPrintableRegions = false;
    bool supportsVertexColorForPrintableRegions = false;
    bool supportsTriangleColorForPrintableRegions = false;
    bool supportsDiscreteComponents = true;
    bool supportsDiscreteObjects = true;
    bool supportsBuildItems = true;
    bool allowsProprietaryTags = false;
};
```

**Relationships**:
- Derived from selected target mode (`None`, `PrusaSlicer`, `Orca`)
- Drives `CompatibilityDecision`

### CompatibilityDecision

Planner output describing the chosen export representation.

```cpp
enum class ExportRepresentation
{
    StandardTexture,
    StandardVertexColor,
    StandardTriangleColor,
    StandardDiscreteComponents,
    StandardDiscreteObjects,
    StandardBuildItems,
    ProprietaryTargetTagged
};

struct CompatibilityDecision
{
    ExportRepresentation finalRepresentation;
    bool needsQuantization = false;
    bool needsRegionization = false;
    bool needsProprietaryTags = false;
    std::vector<std::string> warnings;
};
```

**Invariants**:
- `needsProprietaryTags == true` only when `targetApplication != None`
- `StandardDiscreteComponents` and `StandardDiscreteObjects` imply palette-driven regions
- `StandardBuildItems` is the lowest-fidelity standards-only fallback before the export becomes proprietary
- Warnings are emitted whenever fidelity is reduced or portability is narrowed

### QuantizedPalette

Discrete palette used when gradients or many unique colors must be reduced into assignable printable regions.

```cpp
struct QuantizedPalette
{
    std::vector<Color8> colors;
    std::vector<std::uint32_t> sourceToPaletteMap;
    float maxApproximationError = 0.0f;
};
```

**Validation rules**:
- `colors` must be non-empty when quantization is active
- `sourceToPaletteMap.size()` must match the number of sampled source color records
- Mapping must be deterministic for the same inputs and settings

### PrintableRegion

A discrete printable unit generated during standards-first fallback.

```cpp
struct PrintableRegion
{
    std::uint32_t regionId;
    std::uint32_t paletteIndex;
    std::vector<std::uint32_t> triangleIndices;
    PrintableRegionKind kind; // component, object, or build item
};
```

**Relationships**:
- One `PrintableRegion` references exactly one palette color
- Many triangles may belong to one region
- Region kind determines whether export emits components or standalone objects

### ColoredMeshExportResult

Final export-state record produced by the pipeline.

```cpp
struct ColoredMeshExportResult
{
    ExportRepresentation representation;
    bool standardsOnly = true;
    bool transparencyIgnored = false;
    std::vector<std::string> warnings;
    std::optional<QuantizedPalette> palette;
    std::vector<PrintableRegion> regions;
};
```

## State Transitions

```text
Sample Source Colors
        │
        ▼
Standards-First Attempt
        │
        ├── success with printable regions ──▶ Write standard 3MF
        │
        └── insufficient for printable regions
                    │
                    ▼
            Adaptive Quantization
                    │
                    ▼
            Regionization Planning
                    │
                    ├── standard component/object success ──▶ Write standard 3MF
                    │
                    └── still insufficient
                              │
                              ▼
                        Build-Item Fallback
                            │
                    ├── success ──▶ Write standard 3MF
                    └── still insufficient
                            │
                            ▼
                   Target Application Selected?
                              │
                    ├── no  ──▶ Export warning / standards-limited result
                    └── yes ──▶ Add proprietary target-specific tags
```

## Data Ownership

- `MeshExportDialog` owns `MeshColorExportSettings` while the dialog is active
- `MeshExporter3mf` owns the immutable export-time snapshot of settings
- Quantization and regionization artifacts are transient export objects
- `Writer3mfBase` owns final metadata/tag emission decisions at write time
- Warning text for standards-limited fallback and ignored transparency is owned by exporter/planner output and surfaced by UI/export reporting

## Failure Conditions

- No color source present → export continues as uncolored mesh
- Quantization impossible under configured constraints → planner emits warning and either returns standards-limited result or escalates to selected target mode
- Proprietary tagging requested without target application → invalid configuration
- Regionization produces empty regions → export must fail that fallback path and try the next deterministic option
- Alpha/transparency present → export continues, alpha is ignored for printable-region planning, and a warning is emitted
