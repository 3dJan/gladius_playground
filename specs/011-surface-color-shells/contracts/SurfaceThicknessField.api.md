# SurfaceThicknessField API Contract

**Component**: Surface-Aligned Shell Thickness Field  
**Version**: 1.0.0

## Overview

The `SurfaceThicknessField` class provides a 3D voxel grid storing precomputed shell thicknesses derived from surface colors. It decouples color sampling (at the surface) from shell extraction (at any point).

## Public Interface

### Configuration

```cpp
struct SurfaceThicknessFieldConfig
{
    int gridResolution = 128;           // Voxels per axis (8MB at 128³)
    float seedBandWidth = 1.5f;         // Voxel units for initial seed band
    int maxPropagationDistance = 0;     // Max propagation iterations (0 = auto)
    bool useOpenVdbDilation = true;     // Use OpenVDB for propagation (faster)
    float defaultThickness = 0.0f;      // Unassigned voxel default
};
```

### Class Definition

```cpp
class SurfaceThicknessField
{
public:
    // Construction
    SurfaceThicknessField() = default;
    
    // Build the thickness field from surface data
    // Preconditions:
    //   - vertices.size() == colors.size()
    //   - thicknessLut.size() == lutResolution³
    //   - modelBounds is valid (non-empty)
    // Postconditions:
    //   - isBuilt() returns true
    //   - getFieldBuffer() contains thickness data
    // Throws: std::runtime_error on precondition violation
    void build(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<Eigen::Vector3f> const& colors,
        std::vector<float> const& thicknessLut,
        int lutResolution,
        BBox const& modelBounds,
        SurfaceThicknessFieldConfig const& config = {});
    
    // Query state
    [[nodiscard]] bool isBuilt() const noexcept;
    [[nodiscard]] int getResolution() const noexcept;
    
    // Get raw buffer for GPU upload
    [[nodiscard]] std::vector<float> const& getFieldBuffer() const noexcept;
    
    // Get world→grid transformation matrix (row-major 4x4)
    [[nodiscard]] Eigen::Matrix4f const& getWorldToGridTransform() const noexcept;
    
    // Sample thickness at world position (CPU-side)
    // Returns: Interpolated thickness, or defaultThickness if outside bounds
    [[nodiscard]] float sampleAt(Eigen::Vector3f const& worldPos) const;
};
```

## GPU Interface

### Kernel Signature

```c
__kernel void sampleCornersWithThicknessField(
    __global const float4* positions,    // World positions to sample
    __global float* values,              // Output: SDF offsets
    const unsigned int count,            // Position count
    PAYLOAD_ARGS,                        // Standard model args
    __global const float* thicknessField, // From getFieldBuffer()
    const int fieldResolution,           // From getResolution()
    const float16 worldToGrid            // From getWorldToGridTransform()
);
```

### Buffer Layout

```
thicknessField: float[resolution³]
  - Index(x,y,z) = z * resolution² + y * resolution + x
  - Values in mm (physical thickness units)
  
worldToGrid: float16 (row-major 4x4 matrix)
  - .s0123 = row 0 (transform x)
  - .s4567 = row 1 (transform y)
  - .s89ab = row 2 (transform z)
  - .scdef = row 3 (unused, identity)
```

## Integration Points

### ShellGenerator

```cpp
// ShellGenerator.h - add member
class ShellGenerator
{
    // ...existing...
    
    std::vector<ShellMesh> generateShellsWithSurfaceSampling(
        FilamentStack const& stack,
        ManifoldDualContouringOptions const& options,
        int lutResolution,
        std::vector<std::vector<float>> const& precomputedLuts);
};
```

### HierarchicalOctreeBuilder

```cpp
// HierarchicalDualContouring.h - config extension
struct HierarchicalConfig
{
    // ...existing fields...
    
    SurfaceThicknessField const* thicknessField = nullptr;  // Optional
};
```

## Error Handling

| Condition | Response |
|-----------|----------|
| vertices.size() != colors.size() | Throws `std::runtime_error` |
| Empty vertices array | Returns immediately, isBuilt() = false |
| Invalid bounds | Throws `std::runtime_error` |
| GPU buffer creation fails | Propagates OpenCL exception |

## Performance Contracts

| Operation | Time Complexity | Space Complexity |
|-----------|----------------|------------------|
| build() | O(V + R³) | O(R³) |
| sampleAt() | O(1) | O(1) |
| GPU upload | O(R³) | O(R³) |

Where V = vertex count, R = grid resolution.

## Memory Budget

| Resolution | Buffer Size | Recommended For |
|------------|-------------|-----------------|
| 64³ | ~1 MB | Small models, low memory |
| 128³ | ~8 MB | Default, most models |
| 256³ | ~64 MB | Large, detailed models |
| 512³ | ~512 MB | Maximum precision |
