# Shell-Based Color Export Feature Overview

## Summary
The shell-based color export feature in Gladius allows exporting 3D models as multiple nested shell layers, each with a solid filament color. This is designed for HueForge-style multi-color 3D printing, where different colored filaments are layered to create a full-color appearance when illuminated.

## Key Components

### 1. ColorToThicknessDialog (`gladius/src/ui/ColorToThicknessDialog.h/cpp`)
- Interactive dialog for experimenting with color-to-shell-thickness mapping
- Allows users to define a filament stack (materials with optical properties)
- Computes optimal thickness for each layer to reproduce target colors
- Supports precomputing Look-Up Tables (LUTs) for efficient export
- Key methods:
  - `setPaletteColors()` - Set available colors for mapping
  - `ensurePrecomputedLuts()` - Generate or verify LUTs exist
  - `getPrecomputedLuts()` - Get the computed LUTs
  - `getFilamentStack()` - Get the defined filament materials

### 2. FrontlitThicknessSolver (`gladius/src/io/3mf/FrontlitThicknessSolver.h`)
- Physics-based solver for color reproduction using layered translucent materials
- Solves the inverse problem: given a target color, find layer thicknesses
- Uses iterative optimization to minimize color error
- Supports both frontlit and backlit illumination models

### 3. FaceThicknessMapper (`gladius/src/io/3mf/FaceThicknessMapper.h/cpp`)
- Maps per-face colors to per-face layer thicknesses
- Uses FrontlitThicknessSolver for each face independently
- Supports spatial smoothing to reduce seams between adjacent faces
- Outputs per-layer thickness values for all faces

### 4. ShellGenerator (`gladius/src/io/3mf/ShellGenerator.h/cpp`)
- Generates nested shell meshes from volumetric data
- Uses Manifold Dual Contouring (MDC) to extract isosurfaces
- Supports two modes:
  - **Constant thickness**: Uses fixed thickness per layer from ThicknessSolution
  - **Variable thickness (LUT-based)**: Uses precomputed RGB→thickness LUT for spatially varying thickness
- Creates one mesh per shell interface (outermost first)

### 5. MeshWriter3mf (`gladius/src/io/3mf/MeshWriter3mf.h/cpp`)
- Exports meshes to 3MF format
- Method `exportMeshesWithMaterialColors()` handles shell export
- Each shell mesh gets a solid material color assignment
- Creates multiple build items in the 3MF file (one per shell)

### 6. MeshExportDialog Integration (`gladius/src/ui/MeshExportDialog.h/cpp`)
- UI checkbox: "Export shells with LUT"
- Only enabled for 3MF format + MDC method + volumetric color available
- Button: "Color → Shell Thickness..." opens ColorToThicknessDialog
- Shows LUT status and layer count when ready

## Workflow

### User Workflow
1. Create a model with volumetric color function in Gladius
2. Open the Mesh Export dialog
3. Click "Color → Shell Thickness..." button
4. In ColorToThicknessDialog:
   - Define filament materials (colors, transmittance, reflectance)
   - Set palette colors (target colors to reproduce)
   - Set thickness constraints (min/max per layer)
   - Click compute to generate LUTs
5. Back in export dialog, enable "Export shells with LUT"
6. Export creates multiple colored shell meshes in one 3MF file

### Technical Workflow
1. **Palette Definition**: User defines target colors (e.g., from image, auto-detected, manual)
2. **Material Stack**: User defines filament properties (bottom-to-top order)
3. **LUT Generation**: 
   - For each RGB color in a 3D grid (e.g., 16³ = 4096 colors)
   - Solve for optimal layer thicknesses
   - Store cumulative thickness for each layer
4. **Shell Generation**:
   - For each layer interface (working from outside in)
   - Look up cumulative thickness from LUT based on volumetric color at each point
   - Run MDC at that iso-value to extract the shell surface
5. **Export**:
   - Each shell gets the material color of its corresponding filament
   - Export all shells as separate build items in one 3MF file

## Key Data Structures

### FilamentOpticalProperties
```cpp
struct FilamentOpticalProperties {
    Eigen::Vector3f reflectanceColor;  // Surface color in linear RGB
    Eigen::Vector3f transmittanceColor; // Color of transmitted light
    float baseTransmittance;            // Fraction of light transmitted per mm
    std::string name;                   // Filament identifier
};
```

### ThicknessSolution
```cpp
struct ThicknessSolution {
    std::vector<float> thicknesses;    // Thickness for each layer (mm)
    Eigen::Vector3f achievedColor;     // Resulting color
    float colorError;                  // Error vs target
    bool converged;
};
```

### ShellMesh
```cpp
struct ShellMesh {
    std::vector<Eigen::Vector3f> vertices;
    std::vector<uint32_t> indices;
    std::string filamentName;
    int layerIndex;                    // 0 = bottom layer
};
```

## Configuration Options

### In ColorToThicknessDialog:
- **Filament stack**: Ordered list of materials (bottom to top)
- **Palette**: Target colors to reproduce
- **Thickness constraints**: Min/max thickness per layer, quantization
- **Illumination mode**: Frontlit (HueForge) or Backlit (lithophane)
- **LUT resolution**: Grid size for color sampling (16³, 32³, etc.)

### In MeshExportDialog:
- **Enable shell export**: Checkbox to activate feature
- **Surface extraction method**: Must be "Manifold Dual Contouring (GPU)"
- **Output format**: Must be 3MF
- **Quality settings**: Grid size, adaptivity, projection, simplification

## Requirements

For shell-based export to be available:
1. Output format must be 3MF
2. Surface extraction method must be Manifold Dual Contouring (GPU)
3. Volumetric color must be available in the document
4. Materials must be defined in ColorToThicknessDialog (or will be auto-generated at export)

## Export Output

The exported 3MF file contains:
- Multiple mesh objects (one per shell interface)
- Each mesh named as "Shell_L{layerIndex}_{filamentName}"
- Each mesh assigned a solid material color
- Multiple build items (one per shell)
- Optional thumbnail from document

Example 3MF structure:
```
Shell_L0_White.stl     (innermost/bottom layer)
Shell_L1_Red.stl       
Shell_L2_Blue.stl      (outermost layer)
```

## Related Plans
See `gladius/thegreatplan/color_aware_3mf_export.md` for comprehensive design documentation.
