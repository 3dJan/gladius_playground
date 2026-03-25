# Compatibility Decision API Contract

**Component**: Standards-First Color Compatibility Planner  
**Version**: 1.0.0

## Overview

This contract defines the planner output that decides how a colored mesh export will be represented.

## Public Types

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

## Planner Inputs

- `MeshColorExportSettings`
- source color sample set
- target compatibility profile
- capabilities of currently implemented writer paths

## Required Decision Order

1. Attempt highest-fidelity standard representation allowed by policy and implementation.
2. If printable regions are not preserved, try lower-fidelity standard representations.
3. If needed, enable adaptive quantization.
4. If needed, create discrete standard components/objects.
5. If needed, fall back to standard build-item separation.
6. If standard-only still cannot satisfy the selected policy and `targetApplication != None`, allow proprietary tagging for the selected target.

## Invariants

- `needsProprietaryTags` MUST be false when target application is `None`.
- `finalRepresentation == ProprietaryTargetTagged` MUST imply explicit user target selection.
- `finalRepresentation == StandardBuildItems` MUST remain a standards-only result.
- Warnings MUST be emitted whenever fidelity or portability is reduced.
- Decisions MUST be deterministic under identical inputs.

## Writer Handoff Contract

### Standard Paths
- `StandardTexture`
- `StandardVertexColor`
- `StandardTriangleColor`
- `StandardDiscreteComponents`
- `StandardDiscreteObjects`
- `StandardBuildItems`

These paths MUST be writable without proprietary slicer-specific tagging.

### Proprietary Path
- `ProprietaryTargetTagged`

This path:
- MUST be isolated to one selected target application
- MUST NOT silently activate
- MAY include metadata, object annotations, or package edits specific to the selected slicer

## Error Handling

| Condition | Response |
|-----------|----------|
| No standard path available, no target app selected | Return standards-limited decision with warning |
| Target app selected but no proprietary strategy implemented | Fail export start with clear message |
| Empty source colors after sampling | Degrade to uncolored export |
| Transparency/alpha present | Ignore alpha for printable-region planning and emit a warning |
