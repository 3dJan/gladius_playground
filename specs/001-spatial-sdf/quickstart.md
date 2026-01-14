# Quickstart: Spatial Tree Mesh SDF

**Feature**: 001-spatial-sdf  
**Date**: 2025-12-29

## Overview

This feature adds an alternative mesh-to-SDF computation using a BVH (Bounding Volume Hierarchy) with weighted pseudo-normal sign determination. It replaces the OpenVDB/NanoVDB-based approach for faster startup and better OpenCL compatibility.

## Building

Use the standard VS Code build task:

```
Task: "Build ALL (linux-releaseWithDebug)"
```

No new dependencies required—the feature uses existing gladius infrastructure.

## Running Tests

### Unit Tests (CPU)

```bash
# Run all MeshBVH tests
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=MeshBVH_*

# Run SpatialMeshResource tests
./gladius_test --gtest_filter=SpatialMeshResource_*
```

### GPU Integration Tests

```bash
# Enable GPU tests
export GLADIUS_RUN_GPU_TESTS=1

# Run mesh SDF accuracy tests
./gladius_test --gtest_filter=MeshSDF_*
```

### Full Test Suite

Use the VS Code task:
```
Task: "Run Gladius Tests (linux-releaseWithDebug)"
```

## Usage

### From Code (API)

```cpp
#include "MeshBVH.h"
#include "SpatialMeshResource.h"

// Build spatial mesh from raw data
std::vector<Vector3> vertices = { /* ... */ };
std::vector<Vector3i> indices = { /* ... */ };

gladius::MeshBVHBuilder builder;
auto spatialData = builder.build(vertices, indices);

// Create resource
auto key = ResourceKey{meshId, ResourceType::SpatialMesh};
auto resource = std::make_unique<SpatialMeshResource>(key, std::move(spatialData));

// Add to resource manager
resourceManager.addResource(std::move(resource));
```

### From 3MF File

When loading a 3MF file with a `SignedDistanceToMesh` node, the spatial backend is used automatically if:

1. The mesh is available in the file
2. No explicit VDB grid is provided
3. The configuration prefers spatial backend (default)

```xml
<!-- Existing 3MF implicit function node - no changes needed -->
<i:mesh identifier="distanceToMesh1" objectid="1">
  <i:input identifier="pos" />
  <i:output identifier="distance" />
</i:mesh>
```

## Validation Checklist

After implementation, verify:

- [ ] **SC-001**: Time to first frame < 500ms for 50K triangle mesh
- [ ] **SC-002**: Viewport preview ≥ 10 FPS at 1080p for 100K triangles
- [ ] **SC-003**: Signed distance accuracy within 0.1% of brute-force
- [ ] **SC-004**: Sign correct for 99.9% of points on Stanford Bunny
- [ ] **SC-005**: No OpenCL errors on Intel, AMD, NVIDIA, Rusticl
- [ ] **SC-006**: Memory overhead < 3x raw triangle data

## Troubleshooting

### "Spatial mesh resource not found"

The mesh referenced by `objectid` doesn't have spatial data. Check:
- Mesh exists in the 3MF file
- Mesh has non-zero triangles
- Resource was loaded before compilation

### Sign is incorrect at surface

For watertight meshes with correct results:
- Verify consistent winding order (all CCW or all CW)
- Check for self-intersections
- Ensure mesh is truly closed (no boundary edges)

For non-watertight meshes:
- Sign is undefined; use `unsignedmesh` node instead

### Performance is slow

Check BVH quality:
```cpp
auto stats = builder.getLastBuildStats();
std::cout << "Avg primitives/leaf: " << stats.avgPrimitivesPerLeaf << "\n";
std::cout << "Max depth: " << stats.maxDepth << "\n";
```

Good values: 1-4 primitives/leaf, depth 10-20 for typical meshes.

## File Locations

| Component | Path |
|-----------|------|
| BVH Builder | `gladius/src/MeshBVH.h`, `gladius/src/MeshBVH.cpp` |
| Resource | `gladius/src/SpatialMeshResource.h`, `gladius/src/SpatialMeshResource.cpp` |
| OpenCL Kernel | `gladius/src/kernel/mesh_sdf.cl` |
| Type Definitions | `gladius/src/kernel/types.h` |
| Unit Tests | `gladius/tests/unittests/MeshBVH_tests.cpp` |
| GPU Tests | `gladius/tests/unittests/MeshSDF_tests.cpp` |
