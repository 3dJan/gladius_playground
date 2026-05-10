# Data Model: Surface-Aligned Color Sampling for Shell Generation

**Feature**: 011-surface-color-shells  
**Date**: 2026-01-09

## Overview

This document defines the data structures for the surface-aligned thickness field approach. The key insight is that we precompute a 3D thickness field from surface colors, then use this field during GPU-based shell extraction.

## Entity Relationship Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Shell Generation Pipeline                         │
└─────────────────────────────────────────────────────────────────────────────┘

┌──────────────────┐     ┌──────────────────┐     ┌──────────────────────────┐
│   FilamentStack  │────▶│   ThicknessLUT   │────▶│  SurfaceThicknessField   │
│                  │     │  (RGB → float)   │     │                          │
│ • filaments[]    │     │                  │     │ • fieldBuffer (3D)       │
│ • constraints    │     │ • resolution     │     │ • resolution             │
└──────────────────┘     │ • entries[]      │     │ • worldToGrid transform  │
                         └──────────────────┘     └────────────┬─────────────┘
                                                               │
                                                               ▼
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────────────┐
│ Outer Surface    │────▶│ Vertex Colors    │────▶│ GPU Shell Extraction     │
│ Mesh             │     │ (at surface)     │     │                          │
│                  │     │                  │     │ • sampleCornersWithField │
│ • vertices[]     │     │ • colors[]       │     │ • thicknessField buffer  │
│ • normals[]      │     │ • thicknesses[]  │     │ • fieldResolution        │
│ • indices[]      │     └──────────────────┘     └──────────────────────────┘
└──────────────────┘
```

## Core Data Structures

### SurfaceThicknessFieldConfig

Configuration for building the thickness field.

```cpp
/// @brief Configuration for surface-based thickness field generation
struct SurfaceThicknessFieldConfig
{
    /// Grid resolution per axis (e.g., 128 → 128³ = 2M voxels)
    /// Higher = more accurate, more memory
    /// Default chosen to balance accuracy and memory (~8MB for floats)
    int gridResolution = 128;
    
    /// Narrow band width for surface seeding (in voxels)
    /// Vertices are rasterized within this band around their position
    float seedBandWidth = 1.5f;
    
    /// Maximum propagation distance (in voxels)
    /// If 0, auto-compute from maximum expected shell thickness
    int maxPropagationDistance = 0;
    
    /// Use OpenVDB morphological dilation vs simple flood fill
    /// OpenVDB is faster for large grids, flood fill is simpler
    bool useOpenVdbDilation = true;
    
    /// Default (unassigned) thickness value
    /// Used for voxels that don't receive propagated values
    float defaultThickness = 0.0f;
};
```

### SurfaceThicknessField

The main class encapsulating the precomputed thickness field.

```cpp
/// @brief Precomputed 3D thickness field for GPU shell extraction
///
/// This class builds a dense 3D grid where each voxel contains the
/// cumulative thickness value derived from the surface color at that location.
/// Surface vertices seed the field, and values propagate inward via dilation.
class SurfaceThicknessField
{
public:
    SurfaceThicknessField() = default;
    
    /// @brief Build the thickness field from surface mesh data
    ///
    /// @param vertices Surface mesh vertex positions (on SDF=0)
    /// @param colors Volumetric colors sampled at each vertex (linear RGB)
    /// @param thicknessLut Precomputed LUT mapping RGB → cumulative thickness
    /// @param lutResolution Resolution of the thickness LUT per axis
    /// @param modelBounds Bounding box of the model
    /// @param config Field generation configuration
    ///
    /// @throws std::runtime_error if vertices/colors size mismatch
    void build(std::vector<Eigen::Vector3f> const& vertices,
               std::vector<Eigen::Vector3f> const& colors,
               std::vector<float> const& thicknessLut,
               int lutResolution,
               BBox const& modelBounds,
               SurfaceThicknessFieldConfig const& config = {});
    
    /// @brief Check if field has been built
    [[nodiscard]] bool isBuilt() const noexcept;
    
    /// @brief Get the thickness field as a flat buffer for GPU upload
    ///
    /// Layout: fieldBuffer[z * res² + y * res + x]
    /// Size: resolution³ floats
    [[nodiscard]] std::vector<float> const& getFieldBuffer() const noexcept;
    
    /// @brief Get the grid resolution (same for all axes)
    [[nodiscard]] int getResolution() const noexcept;
    
    /// @brief Get the world-to-grid transformation matrix
    ///
    /// Transform a world position to grid coordinates:
    ///   gridPos = worldToGrid * worldPos
    /// Grid coordinates are in [0, resolution-1] for points inside the bbox.
    [[nodiscard]] Eigen::Matrix4f const& getWorldToGridTransform() const noexcept;
    
    /// @brief Get the grid-to-world transformation matrix (inverse)
    [[nodiscard]] Eigen::Matrix4f const& getGridToWorldTransform() const noexcept;
    
    /// @brief Get memory usage in bytes
    [[nodiscard]] std::size_t getMemoryUsage() const noexcept;
    
    /// @brief Sample thickness at a world position (CPU, for testing)
    [[nodiscard]] float sampleAt(Eigen::Vector3f const& worldPos) const;

private:
    /// Rasterize vertex thicknesses into the grid (seeds the field)
    void rasterizeSurfaceVertices(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<float> const& vertexThicknesses);
    
    /// Propagate thickness values inward using dilation
    void propagateInward(int maxIterations);
    
    /// Propagate using OpenVDB morphological dilation
    void propagateWithOpenVdb(int maxIterations);
    
    /// Propagate using simple flood fill (fallback)
    void propagateWithFloodFill(int maxIterations);
    
    /// Look up thickness from LUT given a color
    [[nodiscard]] float lookupThickness(
        Eigen::Vector3f const& color,
        std::vector<float> const& lut,
        int lutResolution) const;

    std::vector<float> m_fieldBuffer;      ///< Dense 3D grid (res³ floats)
    std::vector<bool> m_assignedMask;      ///< Track which voxels have values
    int m_resolution = 0;
    BBox m_bounds;
    Eigen::Matrix4f m_worldToGrid = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f m_gridToWorld = Eigen::Matrix4f::Identity();
    bool m_isBuilt = false;
};
```

### Extended ShellExportConfig

Modify existing config to include surface thickness field options.

```cpp
/// @brief Configuration for shell-based color export (EXTENDED)
struct ShellExportConfig
{
    // ... existing fields ...
    FilamentStack filamentStack;
    std::vector<std::vector<float>> precomputedLuts;
    int lutResolution = 16;
    ThicknessConstraints thicknessConstraints;
    ManifoldDualContouringOptions mdcOptions;
    
    // NEW: Surface thickness field options
    bool useSurfaceColorSampling = true;  ///< Enable new surface-based sampling
    SurfaceThicknessFieldConfig fieldConfig;  ///< Field generation config
};
```

## GPU Data Structures

### Thickness Field Buffer

The thickness field is uploaded to the GPU as a 1D buffer representing a 3D grid:

```c
// OpenCL buffer layout
// Index calculation: idx = z * resolution² + y * resolution + x
__global const float* thicknessField;  // Size: resolution³

// Trilinear sampling function
float sampleThicknessField(
    __global const float* field,
    int resolution,
    float3 gridPos)
{
    // Clamp to valid range
    gridPos = clamp(gridPos, 0.0f, (float)(resolution - 1));
    
    // Integer and fractional parts
    int3 i = convert_int3(floor(gridPos));
    float3 f = gridPos - convert_float3(i);
    
    // Clamp indices
    i = clamp(i, 0, resolution - 2);
    
    // Sample 8 corners
    #define IDX(x, y, z) ((z) * resolution * resolution + (y) * resolution + (x))
    
    float c000 = field[IDX(i.x, i.y, i.z)];
    float c001 = field[IDX(i.x, i.y, i.z + 1)];
    float c010 = field[IDX(i.x, i.y + 1, i.z)];
    float c011 = field[IDX(i.x, i.y + 1, i.z + 1)];
    float c100 = field[IDX(i.x + 1, i.y, i.z)];
    float c101 = field[IDX(i.x + 1, i.y, i.z + 1)];
    float c110 = field[IDX(i.x + 1, i.y + 1, i.z)];
    float c111 = field[IDX(i.x + 1, i.y + 1, i.z + 1)];
    
    #undef IDX
    
    // Trilinear interpolation
    float c00 = mix(c000, c001, f.z);
    float c01 = mix(c010, c011, f.z);
    float c10 = mix(c100, c101, f.z);
    float c11 = mix(c110, c111, f.z);
    
    float c0 = mix(c00, c01, f.y);
    float c1 = mix(c10, c11, f.y);
    
    return mix(c0, c1, f.x);
}
```

### World-to-Grid Transform

Passed to GPU as a float16 (4x4 matrix):

```c
// Transform world position to grid coordinates
float3 transformToGrid(float16 worldToGrid, float3 worldPos)
{
    float4 homogeneous = (float4)(worldPos, 1.0f);
    
    float4 result;
    result.x = dot(worldToGrid.s0123, homogeneous);
    result.y = dot(worldToGrid.s4567, homogeneous);
    result.z = dot(worldToGrid.s89ab, homogeneous);
    // result.w = dot(worldToGrid.scdef, homogeneous);  // Not needed
    
    return result.xyz;
}
```

## Memory Budget Analysis

| Grid Resolution | Voxel Count | Float Buffer | With Mask | Notes |
|-----------------|-------------|--------------|-----------|-------|
| 64³ | 262,144 | 1 MB | 1.25 MB | Low quality, fast |
| 128³ | 2,097,152 | 8 MB | 10 MB | **Default**, good balance |
| 256³ | 16,777,216 | 64 MB | 80 MB | High quality, large models |
| 512³ | 134,217,728 | 512 MB | 640 MB | Very high quality, memory intensive |

Recommendation: Use 128³ by default, allow user override for large/detailed models.

## Invariants

1. **Field bounds contain model**: `m_bounds` must fully contain the model's bounding box
2. **All surface vertices rasterized**: Every surface vertex contributes to at least one voxel
3. **Propagation complete**: After build(), all voxels within propagation distance have assigned values
4. **Transform invertible**: `gridToWorld * worldToGrid = Identity`
5. **Non-negative thickness**: All thickness values ≥ 0
