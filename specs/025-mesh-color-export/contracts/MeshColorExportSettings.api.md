# Mesh Color Export Settings API Contract

**Component**: Default Mesh Color Export  
**Version**: 1.0.0

## Overview

This contract defines the export-time settings and handoff between the main mesh export dialog and the mesh 3MF exporter.

## Public Types

```cpp
enum class QuantizationMode
{
    Disabled,
    Adaptive
};

enum class TargetApplication
{
    None,
    PrusaSlicer,
    Orca
};

struct MeshColorExportSettings
{
    bool exportWithColors = true;
    bool convertToSrgb = true;
    ColorMode preferredColorMode = ColorMode::PerFace;
    QuantizationMode quantizationMode = QuantizationMode::Adaptive;
    std::optional<std::uint32_t> maxPaletteSize;
    TargetApplication targetApplication = TargetApplication::None;
};
```

## Dialog Contract

### Inputs
- model has volumetric color? → enables color export controls
- output format is 3MF? → enables color export controls

### Controls
- `Export with colors`
- `Convert to sRGB`
- `Color mode`
- `Quantization mode`
- `Optional max palette size`
- `Target application`

### Required Behavior
- The dialog MUST keep standard-only export as the default.
- Selecting a non-`None` target application MUST display a portability warning.
- Settings MUST be captured before export begins and treated as immutable during the export.
- Transparency/alpha is not a user-selectable compatibility mode and MUST be treated as non-printable appearance data.

## Exporter Contract

### Additive Methods

```cpp
void setExportWithColors(bool exportWithColors);
void setConvertToSrgb(bool convertToSrgb);
void setColorMode(ColorMode mode);
void setQuantizationMode(QuantizationMode mode);
void setMaxPaletteSize(std::optional<std::uint32_t> maxPaletteSize);
void setTargetApplication(TargetApplication targetApplication);
```

### Preconditions
- `maxPaletteSize`, if provided, must be >= 2
- `targetApplication != None` implies proprietary tagging is allowed, not required

### Postconditions
- Exporter stores a complete settings snapshot before sampling/writing
- Same input mesh + same settings => same compatibility decision

## Error Handling

| Condition | Response |
|-----------|----------|
| No volumetric color source | Export continues without colors |
| Invalid palette size | Reject export start with clear message |
| Unsupported target application enum | Reject export start with clear message |
| Alpha/transparency present | Export continues, ignores alpha for printable-region planning, and emits a warning |

## Compatibility Notes

- `TargetApplication::None` is the default and must not emit proprietary tags.
- Target selection narrows compatibility expectations to the selected application only when standards-only export is insufficient.
- If standards-only export is insufficient and `TargetApplication::None` remains selected, the exporter returns the best available standard-only result with a warning instead of silently switching modes.
