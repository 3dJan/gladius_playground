# Feature Specification: Spatial Tree Mesh SDF

**Feature Branch**: `001-spatial-sdf`  
**Created**: 2025-12-29  
**Status**: Draft  
**Input**: User description: "Alternative mesh signed distance computation using spatial tree with weighted normal method for real-time OpenCL queries"

## Problem Statement

The current mesh-to-SDF implementation relies on OpenVDB/NanoVDB, which has two significant limitations:

1. **Compatibility Issues**: NanoVDB does not work reliably with some OpenCL implementations (historically problematic with Rusticl and certain GPU drivers)
2. **Build Time**: Converting a mesh to an OpenVDB level set (`openvdb::tools::meshToLevelSet`) is computationally expensive, creating noticeable delays before real-time preview can begin

This feature introduces an alternative spatial acceleration structure with direct mesh-distance queries, eliminating the voxel-grid build phase entirely while maintaining real-time query performance in OpenCL kernels.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Real-time Preview with Mesh SDF (Priority: P1)

A designer loads a 3MF file containing a `SignedDistanceToMesh` node referencing a mesh object. The viewport immediately begins rendering a preview of the implicit function without any perceivable "building grid" delay. As the designer navigates the viewport, the rendering remains smooth and responsive.

**Why this priority**: This is the core value proposition—eliminating the voxel-grid build delay that currently blocks preview. Without this, the feature provides no benefit over the existing NanoVDB path.

**Independent Test**: Can be fully tested by loading a 3MF with a mesh SDF node and measuring time-to-first-frame and render frame rate. Delivers immediate visual feedback to users.

**Acceptance Scenarios**:

1. **Given** a 3MF file with a mesh referenced by `SignedDistanceToMesh` node, **When** the file is opened, **Then** the preview begins rendering within 500ms (excluding file I/O)
2. **Given** an active preview using mesh SDF, **When** the user rotates/pans the viewport, **Then** the frame rate remains above 10 FPS for meshes up to 100K triangles
3. **Given** a mesh with complex topology (e.g., Swiss cheese with internal voids), **When** querying signed distance, **Then** the sign is correct (negative inside, positive outside)

---

### User Story 2 - Fallback-Free OpenCL Compatibility (Priority: P2)

A user with a system that previously showed NanoVDB-related OpenCL errors (e.g., certain AMD drivers, Rusticl) can now use `SignedDistanceToMesh` functionality without encountering runtime failures or needing to switch OpenCL devices.

**Why this priority**: Expanding hardware compatibility enables more users to access mesh SDF functionality. However, P1 must work first to validate the core algorithm.

**Independent Test**: Run the mesh SDF preview on systems known to have NanoVDB issues (e.g., Rusticl environment) and verify no OpenCL errors occur.

**Acceptance Scenarios**:

1. **Given** an OpenCL environment that fails with NanoVDB (e.g., missing features or runtime errors), **When** spatial-tree mesh SDF is used, **Then** signed distance queries complete without OpenCL errors
2. **Given** the spatial-tree mesh SDF is active, **When** switching OpenCL devices, **Then** the feature continues to work on all OpenCL 1.2+ compliant devices

---

### User Story 3 - Mesh Modification Triggers Fast Rebuild (Priority: P3)

When a user modifies the source mesh (e.g., changes referenced mesh object ID, or the mesh itself is updated via API), the spatial acceleration structure rebuilds quickly enough that the user perceives near-instant updates.

**Why this priority**: This enables iterative design workflows. While not essential for initial value, it's important for production use.

**Independent Test**: Modify a mesh via the API while preview is active and measure time to re-render with updated SDF.

**Acceptance Scenarios**:

1. **Given** an active mesh SDF preview, **When** the mesh reference is changed to a different mesh, **Then** the new spatial structure is ready and preview updates within 1 second for meshes up to 50K triangles
2. **Given** a mesh SDF in use, **When** the source mesh vertices are modified, **Then** the system detects the change and triggers a rebuild

---

### Edge Cases

- **Degenerate Triangles**: Meshes with zero-area triangles (collapsed edges) must not cause division-by-zero or NaN results
- **Non-Manifold Meshes**: Open meshes or meshes with holes should produce reasonable unsigned distances; sign determination may be undefined for non-watertight meshes
- **Self-Intersecting Meshes**: The algorithm should return consistent (if not perfectly correct) results; sign may flip at self-intersection regions
- **Empty Mesh**: A mesh with zero triangles should return a large positive distance (infinity/max float)
- **Very Small Meshes**: A mesh with 1-10 triangles should work correctly without tree overhead
- **Large Meshes**: Meshes with 1M+ triangles should not cause memory exhaustion; may trade accuracy for memory

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST compute signed distance from an arbitrary 3D point to a triangle mesh
- **FR-002**: System MUST use a spatial acceleration structure to achieve sub-linear query complexity
- **FR-003**: System MUST determine the sign of the distance using weighted pseudo-normal method (angle-weighted vertex normals evaluated at the closest point)
- **FR-004**: System MUST support queries from OpenCL kernels without host-device synchronization per query
- **FR-005**: System MUST serialize the spatial structure to a flat buffer compatible with existing `PrimitiveBuffer` patterns
- **FR-006**: System MUST work with the existing `SignedDistanceToMesh` node type without changing 3MF file semantics
- **FR-007**: System MUST provide unsigned distance mode for non-watertight meshes (matching existing `unsignedmesh` node behavior)
- **FR-008**: System MUST be selectable as an alternative to the NanoVDB path. Selection logic: (1) spatial backend is preferred when `SpatialMeshResource` is available, (2) automatic fallback to VDB path if spatial resource unavailable, (3) no user configuration required for default behavior

### Key Entities

- **SpatialMeshResource**: Resource containing the mesh geometry and its spatial acceleration structure, analogous to `VdbResource` but without voxel grid
- **MeshBVH**: Bounding Volume Hierarchy over mesh triangles for fast closest-point queries
- **WeightedNormalData**: Per-vertex or per-triangle data needed for sign determination (precomputed angle-weighted normals)
- **ClosestPointResult**: Query result containing distance, closest point, closest primitive (face/edge/vertex), and interpolated normal for sign

## Assumptions

- The input mesh is assumed to be reasonably clean (no degenerate triangles, consistent winding order) for correct sign determination. Non-clean meshes fall back to unsigned distance.
- The mesh fits in GPU memory along with its spatial structure. For very large meshes, users should use the existing voxel-based approach.
- OpenCL 1.2 is the minimum target; no reliance on OpenCL 2.x features (atomics, SVM, etc.)

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Time from mesh load to first rendered frame is under 500ms for a 50K triangle mesh (excluding file I/O)
- **SC-002**: Viewport preview maintains at least 10 FPS during navigation for meshes up to 100K triangles at 1080p resolution
- **SC-003**: Signed distance accuracy is within 0.1% of ground truth (compared to brute-force computation) for test meshes
- **SC-004**: Sign determination is correct for 99.9% of sample points on watertight test meshes (Stanford Bunny, Utah Teapot, etc.)
- **SC-005**: Feature works on all OpenCL 1.2+ devices tested (Intel, AMD, NVIDIA, Rusticl) without runtime errors
- **SC-006**: Memory overhead of spatial structure is less than 3x the raw triangle data size
