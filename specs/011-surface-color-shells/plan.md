# Implementation Plan: Surface-Aligned Color Sampling for Shell Generation

**Branch**: `011-surface-color-shells` | **Date**: 2026-01-09 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/011-surface-color-shells/spec.md`

## Summary

Fix shell generation to sample colors at the **surface** (SDF=0) rather than at interior evaluation points. The solution uses a multi-phase approach: (1) extract outer surface mesh, (2) sample colors at surface vertices, (3) build a thickness field by rasterizing vertex data into a 3D grid, (4) propagate values inward via dilation, (5) GPU kernels read from this precomputed field during shell extraction.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: OpenCL 1.2+ (GPU compute), OpenVDB (sparse grids, morphology), Eigen3 (math)  
**Storage**: N/A (in-memory processing)  
**Testing**: GTest/GMock, integration tests with GPU  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single C++ application with GPU compute  
**Performance Goals**: Shell export within 3x baseline time for 100k triangle equivalent models  
**Constraints**: Must produce watertight meshes, must remain cancellable with progress reporting  
**Scale/Scope**: Models with up to ~1M triangles, LUT resolution up to 64³

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Using C++20, smart pointers, STL algorithms |
| II. Test-First Development | ✅ Pass | Unit tests for new components, integration tests for pipeline |
| III. Simplicity First (KISS/DRY/YAGNI) | ✅ Pass | Reuses existing infrastructure (FaceColorSampler, HierarchicalOctreeBuilder) |
| IV. Consistent Code Style | ✅ Pass | Following Allman braces, naming conventions |
| V. Documentation | ✅ Pass | Doxygen for new public APIs |

## Project Structure

### Documentation (this feature)

```text
specs/011-surface-color-shells/
├── plan.md              # This file
├── research.md          # Approach evaluation
├── data-model.md        # Data structures
├── quickstart.md        # Implementation guide
└── checklists/
    └── requirements.md  # Spec quality checklist
```

### Source Code (modifications)

```text
gladius/src/
├── io/3mf/
│   ├── ShellGenerator.cpp/.h        # MODIFY: Add surface sampling phase
│   ├── SurfaceThicknessField.cpp/.h # NEW: Thickness field builder
│   └── FaceColorSampler.cpp/.h      # EXISTING: Already samples at positions
├── kernel/
│   └── dual_contouring_sampling.cl  # MODIFY: Read from thickness buffer
├── compute/
│   └── ManifoldDualContouringGpu.cpp # MODIFY: Pass thickness buffer to kernel
└── HierarchicalDualContouring.cpp/.h # MODIFY: Support thickness field input

gladius/tests/
├── unittests/
│   └── SurfaceThicknessField_tests.cpp  # NEW: Unit tests
└── integrationtests/
    └── ShellGenerator_tests.cpp         # MODIFY: Add surface sampling tests
```

**Structure Decision**: Modifications to existing shell generation pipeline with one new class (`SurfaceThicknessField`) to encapsulate the thickness field building logic.

## Implementation Phases

### Phase 0: Research ✅
See [research.md](research.md) for detailed approach evaluation.

**Decision**: Hybrid approach combining mesh-based surface sampling with grid-based thickness field propagation.

### Phase 1: Design

#### Data Model

```cpp
/// Configuration for surface-based thickness field generation
struct SurfaceThicknessFieldConfig
{
    int gridResolution = 128;           ///< Voxels per axis for thickness field
    float narrowBandWidth = 2.0f;       ///< How far to propagate (in voxels)
    bool useOpenVdbDilation = true;     ///< Use OpenVDB vs simple flood fill
    int dilationIterations = 0;         ///< If 0, auto-compute from max thickness
};

/// Precomputed thickness field for GPU shell extraction
class SurfaceThicknessField
{
public:
    /// Build from outer surface mesh and color LUT
    /// @param vertices Surface mesh vertices (on SDF=0)
    /// @param colors Sampled colors at each vertex
    /// @param lut Thickness lookup table (color → thickness)
    /// @param bbox Model bounding box
    /// @param config Field configuration
    void build(std::vector<Eigen::Vector3f> const& vertices,
               std::vector<Eigen::Vector3f> const& colors,
               std::vector<float> const& lut,
               int lutResolution,
               BBox const& bbox,
               SurfaceThicknessFieldConfig const& config);
    
    /// Get the field as a flat buffer for GPU upload
    /// @return Dense 3D buffer (gridResolution³ floats)
    std::vector<float> const& getFieldBuffer() const;
    
    /// Get transform from world coords to grid coords
    Eigen::Matrix4f getWorldToGrid() const;
    
    int getResolution() const;
    
private:
    void rasterizeSurfaceVertices();
    void propagateInward();
    
    std::vector<float> m_fieldBuffer;  ///< Dense 3D thickness field
    int m_resolution = 0;
    BBox m_bbox;
    Eigen::Matrix4f m_worldToGrid;
};
```

#### Workflow Changes

```
BEFORE (current):
┌─────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ Build LUT   │───▶│ DC with LUT      │───▶│ Export shells   │
│ (RGB→thick) │    │ (samples color   │    │                 │
└─────────────┘    │  at interior pt) │    └─────────────────┘
                   └──────────────────┘
                          ↑ BUG: wrong color

AFTER (fixed):
┌─────────────┐    ┌──────────────────┐    ┌─────────────────────┐
│ Build LUT   │───▶│ Extract outer    │───▶│ Sample colors at    │
│ (RGB→thick) │    │ surface mesh     │    │ surface vertices    │
└─────────────┘    └──────────────────┘    └──────────┬──────────┘
                                                      │
                   ┌──────────────────────────────────┘
                   ▼
┌──────────────────────┐    ┌──────────────────────┐    ┌─────────────────┐
│ Build thickness      │───▶│ DC with thickness    │───▶│ Export shells   │
│ field (rasterize +   │    │ field (GPU samples   │    │                 │
│ dilate inward)       │    │ from precomputed)    │    └─────────────────┘
└──────────────────────┘    └──────────────────────┘
                                     ↑ FIXED: surface-derived thickness
```

#### GPU Kernel Changes

New kernel that samples from a precomputed thickness buffer:

```c
__kernel void sampleCornersWithThicknessField(
    __global const float4* positions,
    __global float* values,
    const unsigned int count,
    PAYLOAD_ARGS,
    __global const float* thicknessField,  // NEW: precomputed thickness
    const int fieldResolution,
    const float16 worldToGrid              // Transform matrix
)
{
    const int gid = get_global_id(0);
    if (gid >= count) return;
    
    const float4 pos = positions[gid];
    const float3 worldPos = (float3)(pos.x, pos.y, pos.z);
    
    // Sample SDF only (no color needed)
    const float distance = model(worldPos, PASS_PAYLOAD_ARGS).w;
    
    // Sample thickness from precomputed field
    float3 gridPos = transformPoint(worldToGrid, worldPos);
    float thickness = sampleTrilinear(thicknessField, fieldResolution, gridPos);
    
    // Apply thickness offset
    values[gid] = distance + thickness;
}
```

### Phase 2: Implementation Tasks

| # | Task | Effort | Priority | Depends |
|---|------|--------|----------|---------|
| 1 | Create SurfaceThicknessField class skeleton | 2h | P1 | - |
| 2 | Implement vertex rasterization to 3D grid | 3h | P1 | 1 |
| 3 | Implement inward propagation (flood fill or OpenVDB dilation) | 3h | P1 | 2 |
| 4 | Add new GPU kernel for thickness field sampling | 2h | P1 | 1 |
| 5 | Modify ShellGenerator to use new pipeline | 3h | P1 | 3, 4 |
| 6 | Write unit tests for SurfaceThicknessField | 2h | P1 | 3 |
| 7 | Write integration tests for surface sampling | 2h | P1 | 5 |
| 8 | Performance testing and optimization | 2h | P2 | 7 |
| 9 | Documentation updates | 1h | P3 | 5 |

**Total Estimate**: ~20h

### Risk Assessment

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Grid resolution insufficient | Medium | Low | Allow configurable resolution, default to DC maxDepth |
| Dilation performance slow | Medium | Low | OpenVDB dilation is optimized; fallback to simple flood fill |
| GPU buffer too large | High | Low | Use 128³ default (8MB), warn if model needs higher |
| Self-intersection on high curvature | Medium | Medium | Document limitation; shells are mathematically correct |

## Complexity Tracking

> No constitution violations anticipated.

| Aspect | Complexity | Justification |
|--------|------------|---------------|
| New class (SurfaceThicknessField) | Simple | Single responsibility, ~200 LOC |
| New GPU kernel | Simple | Variant of existing kernel, ~30 LOC |
| Pipeline modification | Moderate | Additional phase, but reuses existing components |

## Post-Design Constitution Check

*GATE: Re-evaluation after Phase 1 design completion.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | SurfaceThicknessField uses Eigen, smart pointers, STL algorithms, east-side const |
| II. Test-First Development | ✅ Pass | Unit tests defined for new class, integration tests for pipeline |
| III. Simplicity First | ✅ Pass | Single new class, reuses FaceColorSampler, HierarchicalOctreeBuilder |
| IV. Consistent Code Style | ✅ Pass | Allman braces, naming conventions followed in quickstart examples |
| V. Documentation | ✅ Pass | API contract in contracts/SurfaceThicknessField.api.md, Doxygen in headers |
| VI. Error Handling | ✅ Pass | Precondition checks with exceptions, clear error messages |

**Design Validation**: No constitution violations. The design:
- Introduces minimal new code (~300 LOC total)
- Reuses existing tested components
- Provides clear API boundaries
- Includes comprehensive test strategy


