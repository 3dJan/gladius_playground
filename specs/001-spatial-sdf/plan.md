# Implementation Plan: Spatial Tree Mesh SDF

**Branch**: `001-spatial-sdf` | **Date**: 2025-12-29 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/001-spatial-sdf/spec.md`

## Summary

Implement an alternative mesh signed distance computation method using a BVH (Bounding Volume Hierarchy) with weighted pseudo-normal sign determination. This replaces the OpenVDB/NanoVDB grid-based approach for `SignedDistanceToMesh` nodes, eliminating the slow grid build phase and improving OpenCL compatibility across drivers (especially Rusticl).

The implementation follows the existing `BeamBVH` pattern for spatial acceleration and integrates with the `PrimitiveBuffer`/`ResourceBase` infrastructure for OpenCL kernel access.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: OpenCL 1.2+, existing gladius compute infrastructure  
**Storage**: In-memory `PrimitiveBuffer` (flat GPU-accessible arrays)  
**Testing**: GTest/GMock, GPU tests gated by `GLADIUS_RUN_GPU_TESTS=1`  
**Target Platform**: Linux (Clang), Windows (MSVC); OpenCL 1.2+ GPU/CPU devices  
**Project Type**: Single project (desktop application + library)  
**Performance Goals**: <500ms to first frame (50K triangles), ≥10 FPS at 1080p (100K triangles)  
**Constraints**: <3x memory overhead vs raw triangle data, no OpenCL 2.x features  
**Scale/Scope**: Meshes up to 1M triangles (with graceful degradation)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Will use C++20, smart pointers, STL algorithms |
| II. Test-First Development | ✅ PASS | Unit tests for BVH builder, accuracy tests for SDF, GPU integration tests |
| III. Simplicity First | ✅ PASS | Follows existing `BeamBVH` pattern; single resource class |
| IV. Consistent Code Style | ✅ PASS | Will follow Allman braces, naming conventions |
| V. Documentation | ✅ PASS | Doxygen comments for public APIs |

**No violations requiring justification.**

## Project Structure

### Documentation (this feature)

```text
specs/001-spatial-sdf/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (internal APIs)
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/src/
├── MeshBVH.h                    # BVH builder for triangle meshes (host-side)
├── MeshBVH.cpp                  # BVH construction with SAH
├── SpatialMeshResource.h        # Resource class for mesh + BVH (analogous to VdbResource)
├── SpatialMeshResource.cpp      # Serialization to PrimitiveBuffer
├── kernel/
│   ├── mesh_sdf.cl              # OpenCL kernel for BVH traversal + closest point
│   └── types.h                  # Add SDF_MESH_BVH_NODE, SDF_MESH_BVH_LEAF primitive types
├── nodes/
│   └── DerivedNodes.h           # Update SignedDistanceToMesh node to use spatial path
└── compute/
    └── ProgramManager.cpp       # Add spatial mesh SDF capability gate

gladius/tests/unittests/
├── MeshBVH_tests.cpp            # Unit tests for BVH construction
├── SpatialMeshResource_tests.cpp# Serialization and integration tests
└── MeshSDF_tests.cpp            # Accuracy and sign correctness tests (GPU)
```

**Structure Decision**: Single project structure. New files follow existing patterns (`BeamBVH.*`, `VdbResource.*`). OpenCL kernel code goes in `kernel/mesh_sdf.cl` following the `sdf.cl` pattern.

## Complexity Tracking

> **No violations requiring justification.**
