# Quickstart: Surface-Aligned Color Sampling for Shell Generation

**Feature**: 011-surface-color-shells  
**Date**: 2026-01-09

## Overview

This document provides implementation guidance for fixing shell color sampling to use surface colors instead of interior colors.

## Architecture Summary

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        Shell Export Pipeline (NEW)                         │
└────────────────────────────────────────────────────────────────────────────┘

  ┌─────────────────┐                    
  │  ShellExporter  │ Entry point (existing)
  └────────┬────────┘
           │
           ▼
  ┌─────────────────┐     ┌─────────────────────┐
  │ ShellGenerator  │────▶│ Phase 1: Extract    │
  │ (modified)      │     │ outer surface mesh  │
  └─────────────────┘     └──────────┬──────────┘
                                     │
                          ┌──────────▼──────────┐
                          │ Phase 2: Sample     │
                          │ colors at vertices  │
                          │ (FaceColorSampler)  │
                          └──────────┬──────────┘
                                     │
                          ┌──────────▼───────────────┐
                          │ Phase 3: Build thickness │
                          │ field (NEW class)        │
                          │ SurfaceThicknessField    │
                          └──────────┬───────────────┘
                                     │
                          ┌──────────▼──────────┐
                          │ Phase 4: Extract    │
                          │ shells using field  │
                          │ (GPU kernel modified)│
                          └──────────┬──────────┘
                                     │
                          ┌──────────▼──────────┐
                          │ Phase 5: Export     │
                          │ to 3MF              │
                          └─────────────────────┘
```

## Key Implementation Steps

### Step 1: Create SurfaceThicknessField Class

**File**: `gladius/src/io/3mf/SurfaceThicknessField.h`

```cpp
#pragma once

#include "BBox.h"
#include "kernel/types.h"
#include <eigen3/Eigen/Core>
#include <vector>

namespace gladius::io
{
    struct SurfaceThicknessFieldConfig
    {
        int gridResolution = 128;
        float seedBandWidth = 1.5f;
        int maxPropagationDistance = 0;
        bool useOpenVdbDilation = false;
        float defaultThickness = 0.0f;
    };

    class SurfaceThicknessField
    {
    public:
        SurfaceThicknessField() = default;

        void build(std::vector<Eigen::Vector3f> const& vertices,
                   std::vector<Eigen::Vector3f> const& colors,
                   std::vector<float> const& thicknessLut,
                   int lutResolution,
                   BoundingBox const& modelBounds,  // Uses BoundingBox (float4-based)
                   SurfaceThicknessFieldConfig const& config = {});

        [[nodiscard]] bool isBuilt() const noexcept { return m_isBuilt; }
        [[nodiscard]] std::vector<float> const& getFieldBuffer() const noexcept { return m_fieldBuffer; }
        [[nodiscard]] int getResolution() const noexcept { return m_resolution; }
        [[nodiscard]] Eigen::Matrix4f const& getWorldToGridTransform() const noexcept { return m_worldToGrid; }
        [[nodiscard]] float sampleAt(Eigen::Vector3f const& worldPos) const;
        [[nodiscard]] std::size_t getMemoryUsage() const noexcept;

    private:
        void rasterizeSurfaceVertices(
            std::vector<Eigen::Vector3f> const& vertices,
            std::vector<float> const& vertexThicknesses);
        void propagateInward(int maxIterations);
        
        [[nodiscard]] float lookupThickness(
            Eigen::Vector3f const& color,
            std::vector<float> const& lut,
            int lutResolution) const;

        std::vector<float> m_fieldBuffer;
        std::vector<bool> m_assignedMask;
        int m_resolution = 0;
        BBox m_bounds;  // Internal storage uses BBox (Eigen-based)
        Eigen::Matrix4f m_worldToGrid = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f m_gridToWorld = Eigen::Matrix4f::Identity();
        bool m_isBuilt = false;
    };
}
```

**File**: `gladius/src/io/3mf/SurfaceThicknessField.cpp`

```cpp
#include "SurfaceThicknessField.h"

#include <openvdb/openvdb.h>
#include <openvdb/tools/Morphology.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

namespace gladius::io
{
    void SurfaceThicknessField::build(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<Eigen::Vector3f> const& colors,
        std::vector<float> const& thicknessLut,
        int lutResolution,
        BBox const& modelBounds,
        SurfaceThicknessFieldConfig const& config)
    {
        if (vertices.size() != colors.size())
        {
            throw std::runtime_error("Vertex and color arrays must have same size");
        }

        m_resolution = config.gridResolution;
        m_bounds = modelBounds;
        
        // Add small padding to bounds
        float const padding = 0.01f * m_bounds.diagonal().norm();
        m_bounds.min() -= Eigen::Vector3f::Constant(padding);
        m_bounds.max() += Eigen::Vector3f::Constant(padding);

        // Compute transforms
        Eigen::Vector3f const size = m_bounds.diagonal();
        Eigen::Vector3f const scale(
            static_cast<float>(m_resolution - 1) / size.x(),
            static_cast<float>(m_resolution - 1) / size.y(),
            static_cast<float>(m_resolution - 1) / size.z());

        m_worldToGrid = Eigen::Matrix4f::Identity();
        m_worldToGrid(0, 0) = scale.x();
        m_worldToGrid(1, 1) = scale.y();
        m_worldToGrid(2, 2) = scale.z();
        m_worldToGrid(0, 3) = -m_bounds.min().x() * scale.x();
        m_worldToGrid(1, 3) = -m_bounds.min().y() * scale.y();
        m_worldToGrid(2, 3) = -m_bounds.min().z() * scale.z();

        m_gridToWorld = m_worldToGrid.inverse();

        // Allocate field
        std::size_t const totalVoxels = static_cast<std::size_t>(m_resolution) *
                                        m_resolution * m_resolution;
        m_fieldBuffer.assign(totalVoxels, config.defaultThickness);
        m_assignedMask.assign(totalVoxels, false);

        // Convert colors to thicknesses
        std::vector<float> vertexThicknesses(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            vertexThicknesses[i] = lookupThickness(colors[i], thicknessLut, lutResolution);
        }

        // Phase 1: Rasterize surface vertices
        rasterizeSurfaceVertices(vertices, vertexThicknesses);

        // Phase 2: Propagate inward
        int const maxPropDist = config.maxPropagationDistance > 0
            ? config.maxPropagationDistance
            : m_resolution / 4;  // Reasonable default
        propagateInward(maxPropDist);

        m_isBuilt = true;
    }

    float SurfaceThicknessField::lookupThickness(
        Eigen::Vector3f const& color,
        std::vector<float> const& lut,
        int lutResolution) const
    {
        // Trilinear interpolation in LUT
        Eigen::Vector3f const uvw = color.cwiseMax(0.0f).cwiseMin(1.0f) * 
                                    static_cast<float>(lutResolution - 1);
        
        Eigen::Vector3i const idx = uvw.cast<int>().cwiseMax(0).cwiseMin(lutResolution - 2);
        Eigen::Vector3f const frac = uvw - idx.cast<float>();

        auto lutIdx = [lutResolution](int r, int g, int b) -> std::size_t {
            return (static_cast<std::size_t>(r) * lutResolution + g) * lutResolution + b;
        };

        // Sample 8 corners
        float const c000 = lut[lutIdx(idx.x(), idx.y(), idx.z())];
        float const c001 = lut[lutIdx(idx.x(), idx.y(), idx.z() + 1)];
        float const c010 = lut[lutIdx(idx.x(), idx.y() + 1, idx.z())];
        float const c011 = lut[lutIdx(idx.x(), idx.y() + 1, idx.z() + 1)];
        float const c100 = lut[lutIdx(idx.x() + 1, idx.y(), idx.z())];
        float const c101 = lut[lutIdx(idx.x() + 1, idx.y(), idx.z() + 1)];
        float const c110 = lut[lutIdx(idx.x() + 1, idx.y() + 1, idx.z())];
        float const c111 = lut[lutIdx(idx.x() + 1, idx.y() + 1, idx.z() + 1)];

        // Trilinear interpolation
        float const c00 = c000 + (c001 - c000) * frac.z();
        float const c01 = c010 + (c011 - c010) * frac.z();
        float const c10 = c100 + (c101 - c100) * frac.z();
        float const c11 = c110 + (c111 - c110) * frac.z();

        float const c0 = c00 + (c01 - c00) * frac.y();
        float const c1 = c10 + (c11 - c10) * frac.y();

        return c0 + (c1 - c0) * frac.x();
    }

    void SurfaceThicknessField::rasterizeSurfaceVertices(
        std::vector<Eigen::Vector3f> const& vertices,
        std::vector<float> const& vertexThicknesses)
    {
        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            // Transform to grid coordinates
            Eigen::Vector4f const homogeneous(vertices[i].x(), vertices[i].y(), 
                                              vertices[i].z(), 1.0f);
            Eigen::Vector4f const gridCoord = m_worldToGrid * homogeneous;
            
            // Round to nearest voxel
            int const x = std::clamp(static_cast<int>(std::round(gridCoord.x())), 
                                     0, m_resolution - 1);
            int const y = std::clamp(static_cast<int>(std::round(gridCoord.y())), 
                                     0, m_resolution - 1);
            int const z = std::clamp(static_cast<int>(std::round(gridCoord.z())), 
                                     0, m_resolution - 1);

            std::size_t const idx = voxelIdx(x, y, z);
            
            // If already assigned, average (for overlapping vertices)
            if (m_assignedMask[idx])
            {
                m_fieldBuffer[idx] = (m_fieldBuffer[idx] + vertexThicknesses[i]) * 0.5f;
            }
            else
            {
                m_fieldBuffer[idx] = vertexThicknesses[i];
                m_assignedMask[idx] = true;
            }
        }
    }

    void SurfaceThicknessField::propagateInward(int maxIterations)
    {
        // Simple flood fill propagation (alternative to OpenVDB)
        // BFS from assigned voxels, propagating thickness values
        
        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        auto isValid = [this](int x, int y, int z) -> bool {
            return x >= 0 && x < m_resolution &&
                   y >= 0 && y < m_resolution &&
                   z >= 0 && z < m_resolution;
        };

        // 6-connected neighbors
        std::array<std::array<int, 3>, 6> const neighbors = {{
            {-1, 0, 0}, {1, 0, 0},
            {0, -1, 0}, {0, 1, 0},
            {0, 0, -1}, {0, 0, 1}
        }};

        // Initialize queue with all assigned voxels
        std::queue<std::tuple<int, int, int, int>> queue;  // x, y, z, distance
        
        for (int z = 0; z < m_resolution; ++z)
        {
            for (int y = 0; y < m_resolution; ++y)
            {
                for (int x = 0; x < m_resolution; ++x)
                {
                    if (m_assignedMask[voxelIdx(x, y, z)])
                    {
                        queue.emplace(x, y, z, 0);
                    }
                }
            }
        }

        // BFS propagation
        while (!queue.empty())
        {
            auto const [x, y, z, dist] = queue.front();
            queue.pop();

            if (dist >= maxIterations)
            {
                continue;
            }

            std::size_t const currentIdx = voxelIdx(x, y, z);
            float const currentThickness = m_fieldBuffer[currentIdx];

            for (auto const& [dx, dy, dz] : neighbors)
            {
                int const nx = x + dx;
                int const ny = y + dy;
                int const nz = z + dz;

                if (!isValid(nx, ny, nz))
                {
                    continue;
                }

                std::size_t const neighborIdx = voxelIdx(nx, ny, nz);

                if (!m_assignedMask[neighborIdx])
                {
                    m_fieldBuffer[neighborIdx] = currentThickness;
                    m_assignedMask[neighborIdx] = true;
                    queue.emplace(nx, ny, nz, dist + 1);
                }
            }
        }
    }

    float SurfaceThicknessField::sampleAt(Eigen::Vector3f const& worldPos) const
    {
        if (!m_isBuilt)
        {
            return 0.0f;
        }

        Eigen::Vector4f const homogeneous(worldPos.x(), worldPos.y(), worldPos.z(), 1.0f);
        Eigen::Vector4f const gridCoord = m_worldToGrid * homogeneous;

        // Clamp to valid range
        Eigen::Vector3f const gridPos(
            std::clamp(gridCoord.x(), 0.0f, static_cast<float>(m_resolution - 1)),
            std::clamp(gridCoord.y(), 0.0f, static_cast<float>(m_resolution - 1)),
            std::clamp(gridCoord.z(), 0.0f, static_cast<float>(m_resolution - 1)));

        // Trilinear interpolation
        Eigen::Vector3i const idx = gridPos.cast<int>().cwiseMax(0).cwiseMin(m_resolution - 2);
        Eigen::Vector3f const frac = gridPos - idx.cast<float>();

        auto voxelIdx = [this](int x, int y, int z) -> std::size_t {
            return (static_cast<std::size_t>(z) * m_resolution + y) * m_resolution + x;
        };

        float const c000 = m_fieldBuffer[voxelIdx(idx.x(), idx.y(), idx.z())];
        float const c001 = m_fieldBuffer[voxelIdx(idx.x(), idx.y(), idx.z() + 1)];
        float const c010 = m_fieldBuffer[voxelIdx(idx.x(), idx.y() + 1, idx.z())];
        float const c011 = m_fieldBuffer[voxelIdx(idx.x(), idx.y() + 1, idx.z() + 1)];
        float const c100 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y(), idx.z())];
        float const c101 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y(), idx.z() + 1)];
        float const c110 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y() + 1, idx.z())];
        float const c111 = m_fieldBuffer[voxelIdx(idx.x() + 1, idx.y() + 1, idx.z() + 1)];

        float const c00 = c000 + (c001 - c000) * frac.z();
        float const c01 = c010 + (c011 - c010) * frac.z();
        float const c10 = c100 + (c101 - c100) * frac.z();
        float const c11 = c110 + (c111 - c110) * frac.z();

        float const c0 = c00 + (c01 - c00) * frac.y();
        float const c1 = c10 + (c11 - c10) * frac.y();

        return c0 + (c1 - c0) * frac.x();
    }
}
```

### Step 2: Add GPU Kernel for Thickness Field Sampling

**File**: `gladius/src/kernel/dual_contouring_sampling.cl` (add new kernel)

```c
/// Sample corners using a precomputed surface-derived thickness field
/// This kernel is used for surface-aligned shell generation
__kernel void sampleCornersWithThicknessField(
    __global const float4* positions,    // Input: (x,y,z,_) positions
    __global float* values,              // Output: SDF values
    const unsigned int count,
    PAYLOAD_ARGS,
    __global const float* thicknessField,  // Precomputed thickness field
    const int fieldResolution,             // Grid resolution per axis
    const float16 worldToGrid              // 4x4 transform matrix (row-major)
)
{
    const int gid = get_global_id(0);
    if (gid >= count) return;

    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);

    // Sample SDF at this position (no color needed!)
    const float distance = model(worldPos, PASS_PAYLOAD_ARGS).w;

    // Transform world position to grid coordinates
    float4 homogeneous = (float4)(worldPos, 1.0f);
    float3 gridPos;
    gridPos.x = dot(worldToGrid.s0123, homogeneous);
    gridPos.y = dot(worldToGrid.s4567, homogeneous);
    gridPos.z = dot(worldToGrid.s89ab, homogeneous);

    // Clamp to valid grid range
    const float maxCoord = (float)(fieldResolution - 1);
    gridPos = clamp(gridPos, 0.0f, maxCoord);

    // Trilinear interpolation
    int3 idx = convert_int3(floor(gridPos));
    idx = clamp(idx, 0, fieldResolution - 2);
    float3 frac = gridPos - convert_float3(idx);

    #define FIELD_IDX(x, y, z) ((z) * fieldResolution * fieldResolution + (y) * fieldResolution + (x))

    float c000 = thicknessField[FIELD_IDX(idx.x, idx.y, idx.z)];
    float c001 = thicknessField[FIELD_IDX(idx.x, idx.y, idx.z + 1)];
    float c010 = thicknessField[FIELD_IDX(idx.x, idx.y + 1, idx.z)];
    float c011 = thicknessField[FIELD_IDX(idx.x, idx.y + 1, idx.z + 1)];
    float c100 = thicknessField[FIELD_IDX(idx.x + 1, idx.y, idx.z)];
    float c101 = thicknessField[FIELD_IDX(idx.x + 1, idx.y, idx.z + 1)];
    float c110 = thicknessField[FIELD_IDX(idx.x + 1, idx.y + 1, idx.z)];
    float c111 = thicknessField[FIELD_IDX(idx.x + 1, idx.y + 1, idx.z + 1)];

    #undef FIELD_IDX

    float c00 = mix(c000, c001, frac.z);
    float c01 = mix(c010, c011, frac.z);
    float c10 = mix(c100, c101, frac.z);
    float c11 = mix(c110, c111, frac.z);

    float c0 = mix(c00, c01, frac.y);
    float c1 = mix(c10, c11, frac.y);

    float thickness = mix(c0, c1, frac.x);

    // Shell extraction: surface at SDF = -thickness
    values[gid] = distance + thickness;
}
```

### Step 3: Modify ShellGenerator

**File**: `gladius/src/io/3mf/ShellGenerator.cpp` (modify `generateShells`)

```cpp
// Add to includes
#include "SurfaceThicknessField.h"
#include "FaceColorSampler.h"

// In generateShells(), add new surface-based path:

std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShells(
    FilamentStack const& stack,
    ThicknessSolution const& solution,
    ManifoldDualContouringOptions const& options,
    int thicknessLutResolution,
    ThicknessConstraints thicknessConstraints,
    std::vector<std::vector<float>> const* precomputedLuts,
    bool useSurfaceColorSampling)  // NEW parameter
{
    // ... existing validation code ...

    if (useSurfaceColorSampling && precomputedLuts != nullptr && !precomputedLuts->empty())
    {
        // NEW: Surface-based sampling path
        return generateShellsWithSurfaceSampling(
            stack, options, thicknessLutResolution, *precomputedLuts);
    }
    else
    {
        // EXISTING: Interior sampling path (kept for fallback/comparison)
        // ... existing code ...
    }
}

std::vector<ShellGenerator::ShellMesh> ShellGenerator::generateShellsWithSurfaceSampling(
    FilamentStack const& stack,
    ManifoldDualContouringOptions const& options,
    int lutResolution,
    std::vector<std::vector<float>> const& precomputedLuts)
{
    std::vector<ShellMesh> shells;

    // Phase 1: Extract outer surface mesh
    hierarchical_dc::HierarchicalConfig outerConfig = toHierarchicalConfig(options);
    outerConfig.isoValue = 0.0f;  // Outer surface
    
    hierarchical_dc::HierarchicalOctreeBuilder builder(m_core, outerConfig);
    builder.buildOctree(m_core.getBoundingBox().value());
    
    std::vector<Eigen::Vector3f> surfaceVertices;
    std::vector<std::uint32_t> surfaceIndices;
    builder.extractMesh(surfaceVertices, surfaceIndices);
    
    if (surfaceVertices.empty())
    {
        return shells;
    }

    // Phase 2: Sample colors at surface vertices
    io::FaceColorSampler sampler(m_core.getContext());
    auto surfaceColors = sampler.sampleVertexColors(surfaceVertices, m_core.getPrimitives());

    // Phase 3-5: Build thickness field and extract shells for each layer
    for (int layerIdx = static_cast<int>(stack.size()) - 1; layerIdx >= 0; --layerIdx)
    {
        // Get LUT for this layer (cumulative thickness from this layer to top)
        if (static_cast<std::size_t>(layerIdx) >= precomputedLuts.size())
        {
            continue;
        }
        
        std::vector<float> const& layerLut = precomputedLuts[layerIdx];
        
        // Build thickness field from surface colors
        SurfaceThicknessField field;
        field.build(surfaceVertices, surfaceColors, layerLut, lutResolution,
                    m_core.getBoundingBox().value());

        // Extract shell mesh using thickness field
        // This requires a new code path in HierarchicalOctreeBuilder
        // that samples from the thickness field instead of the color LUT
        
        hierarchical_dc::HierarchicalConfig shellConfig = outerConfig;
        shellConfig.thicknessField = &field;  // NEW: pass field instead of LUT
        
        hierarchical_dc::HierarchicalOctreeBuilder shellBuilder(m_core, shellConfig);
        shellBuilder.buildOctree(m_core.getBoundingBox().value());
        
        std::vector<Eigen::Vector3f> shellVertices;
        std::vector<std::uint32_t> shellIndices;
        shellBuilder.extractMesh(shellVertices, shellIndices);
        
        if (!shellVertices.empty() && !shellIndices.empty())
        {
            ShellMesh mesh;
            mesh.vertices = std::move(shellVertices);
            mesh.indices = std::move(shellIndices);
            mesh.filamentName = stack[layerIdx].name;
            mesh.layerIndex = layerIdx;
            shells.push_back(std::move(mesh));
        }
    }

    return shells;
}
```

## Testing Strategy

### Unit Tests

```cpp
// File: gladius/tests/unittests/SurfaceThicknessField_tests.cpp

#include <gtest/gtest.h>
#include "io/3mf/SurfaceThicknessField.h"

namespace gladius::io::tests
{
    TEST(SurfaceThicknessField_Test, Build_WithValidInput_Succeeds)
    {
        std::vector<Eigen::Vector3f> vertices = {
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f}
        };
        
        std::vector<Eigen::Vector3f> colors = {
            {1.0f, 0.0f, 0.0f},  // Red
            {0.0f, 1.0f, 0.0f},  // Green
            {0.0f, 0.0f, 1.0f}   // Blue
        };
        
        // Simple LUT: all same thickness
        int const lutRes = 4;
        std::vector<float> lut(lutRes * lutRes * lutRes, 1.0f);
        
        BBox bounds;
        bounds.min() = Eigen::Vector3f(-0.5f, -0.5f, -0.5f);
        bounds.max() = Eigen::Vector3f(1.5f, 1.5f, 0.5f);
        
        SurfaceThicknessField field;
        EXPECT_NO_THROW(field.build(vertices, colors, lut, lutRes, bounds));
        EXPECT_TRUE(field.isBuilt());
        EXPECT_EQ(field.getResolution(), 128);  // Default
    }

    TEST(SurfaceThicknessField_Test, SampleAt_ReturnsInterpolatedValues)
    {
        // Build field with known thickness pattern
        // ...
        // Sample at various positions and verify interpolation
    }

    TEST(SurfaceThicknessField_Test, Build_WithMismatchedArrays_Throws)
    {
        std::vector<Eigen::Vector3f> vertices(3);
        std::vector<Eigen::Vector3f> colors(5);  // Mismatch!
        
        SurfaceThicknessField field;
        EXPECT_THROW(field.build(vertices, colors, {}, 4, BBox{}), std::runtime_error);
    }
}
```

### Integration Tests

```cpp
// File: gladius/tests/integrationtests/ShellGenerator_tests.cpp (add new test)

TEST(ShellGenerator_SurfaceSampling_Test, GenerateShells_UsesCorrectSurfaceColors)
{
    // Load a model with known color pattern (e.g., image projection)
    // Generate shells with surface sampling enabled
    // Verify shell thicknesses match expected surface colors, not interior
}
```

## Migration Notes

- The `useSurfaceColorSampling` flag defaults to `false` for backward compatibility
- Set `useSurfaceColorSampling = true` to enable the new surface-based sampling
- Existing behavior preserved when flag is `false` (for regression testing)
- UI option to toggle between methods for comparison (debug/development)

## Performance Considerations

1. **Grid Resolution**: Default 128³ uses ~8MB, sufficient for most models
2. **Propagation**: BFS flood fill is O(voxels), typically <1 second
3. **GPU Upload**: Single buffer upload of thickness field (~8MB)
4. **Kernel Efficiency**: New kernel avoids color sampling, potentially faster

## Future Optimizations

1. **OpenVDB Dilation**: Replace flood fill with `openvdb::tools::dilateActiveValues` for large grids
2. **NanoVDB**: Use NanoVDB for sparse GPU access if memory becomes an issue
3. **Adaptive Resolution**: Use higher resolution only where needed (near surface detail)
