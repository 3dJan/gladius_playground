# Feature Specification: Mesh SDF Performance Improvements

**Feature Branch**: `002-mesh-sdf-performance`  
**Created**: 2025-12-30  
**Status**: Draft  
**Input**: User description: "Performance improvements for signed distance to mesh computation"

## Overview

This feature focuses on optimizing the performance of signed distance field (SDF) queries against triangle meshes. The current implementation in `mesh_sdf.cl` uses BVH traversal with pseudo-normal sign determination, but there are opportunities to improve query throughput for both rendering and mesh generation workloads.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Faster Mesh Preview Rendering (Priority: P1)

As a designer working with imported mesh geometry, I want the viewport to respond smoothly when manipulating views of complex meshes, so that I can efficiently evaluate my designs without waiting for slow SDF computations.

**Why this priority**: Rendering is the most frequent SDF query operation - every frame requires thousands of queries. Improving rendering throughput has the highest user-visible impact on the application's responsiveness.

**Independent Test**: Can be fully tested by loading a complex mesh (>100K triangles) and measuring viewport frame rate during orbit/pan operations. A measurable improvement in FPS validates this story.

**Acceptance Scenarios**:

1. **Given** a mesh with 100,000+ triangles is loaded, **When** the user orbits the viewport, **Then** the frame rate remains above 30 FPS on mid-range GPUs.
2. **Given** a mesh SDF is evaluated during raymarching, **When** the same spatial region is queried repeatedly (camera stationary), **Then** the evaluation time does not regress compared to the first frame.
3. **Given** multiple mesh SDF nodes are in the scene graph, **When** rendering, **Then** each mesh query benefits independently from the optimizations.

---

### User Story 2 - Accelerated Mesh Export with SDF Operations (Priority: P2)

As a user exporting a model that combines implicit primitives with mesh geometry, I want the mesh export process to complete faster when mesh SDF queries are involved, so that I spend less time waiting for exports.

**Why this priority**: Export operations involve dense SDF sampling across the entire model volume. While less frequent than rendering, export times directly impact productivity for users iterating on designs.

**Independent Test**: Can be tested by timing mesh export operations that involve mesh SDF queries and comparing against baseline performance.

**Acceptance Scenarios**:

1. **Given** a model combining mesh SDF with implicit primitives, **When** exporting via dual contouring, **Then** the export completes at least 20% faster than the baseline (per SC-003).
2. **Given** a large mesh (500K+ triangles), **When** computing SDF samples for export, **Then** the BVH traversal efficiently prunes distant regions.

---

### User Story 3 - Reduced Memory Bandwidth for Mesh SDF (Priority: P3)

As a user with limited GPU memory bandwidth, I want mesh SDF queries to use memory efficiently, so that I can work with larger meshes without hitting bandwidth bottlenecks.

**Why this priority**: Memory bandwidth is often the limiting factor for GPU kernels. Reducing memory traffic improves performance on bandwidth-constrained hardware and allows larger meshes.

**Independent Test**: Can be validated by profiling memory access patterns and measuring reduction in global memory transactions.

**Acceptance Scenarios**:

1. **Given** a mesh with vertex normals stored for sign computation, **When** querying near vertices shared by many faces, **Then** normal data is fetched efficiently without redundant reads.
2. **Given** the BVH node data is accessed during traversal, **When** child nodes are visited, **Then** memory access patterns minimize cache thrashing.

---

### Edge Cases

- What happens when a query point is equidistant from multiple triangles? The implementation correctly handles this by using the first triangle found at minimum distance; the sign determination remains consistent due to pseudo-normal continuity.
- How does the system handle degenerate triangles (zero area) during traversal? The current implementation includes epsilon checks and falls back to vertex distance for degenerate cases.
- What is the behavior for meshes with extremely unbalanced BVH trees? The iteration limit (nodeCount * 2 + 100) prevents infinite loops; performance degrades gracefully.
- How are queries handled when the mesh has sharp creases producing near-zero pseudo-normals? The implementation falls back to face normal when pseudo-normal length is below threshold.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST maintain correctness of signed distance computations after optimization (same results within floating-point tolerance).
- **FR-002**: System MUST preserve the existing BVH traversal safety mechanisms (bounds checking, iteration limits).
- **FR-003**: System MUST support both signed and unsigned distance queries with the optimizations applied.
- **FR-004**: System MUST handle meshes ranging from small (100 triangles) to large (1M+ triangles) without performance regression on either end.
- **FR-005**: System MUST work within the existing OpenCL 1.2 constraints (no recursion, no dynamic allocation).
- **FR-006**: Optimized kernels MUST be compatible with the existing `PrimitiveBuffer` memory layout.
- **FR-007**: Memory usage increase is acceptable if it provides measurable performance improvement.

### Key Entities

- **MeshBVHNodeGPU**: BVH traversal node (48 bytes) - candidates for layout optimization
- **MeshTriangleGPU**: Triangle geometry (48 bytes) - potential for cache-friendly access patterns  
- **MeshVertexNormalGPU**: Vertex normals for sign computation (16 bytes) - shared access patterns
- **ClosestPointResult**: Per-query result structure - register pressure optimization target

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Average mesh SDF query time decreases by at least 20% for meshes with 100K+ triangles on representative GPU hardware.
- **SC-002**: Viewport frame rate improves measurably when rendering scenes dominated by mesh SDF queries.
- **SC-003**: Mesh export operations involving mesh SDF complete in less time than baseline implementation.
- **SC-004**: Memory bandwidth utilization per query decreases, as measured by GPU profiling tools.
- **SC-005**: No correctness regressions in existing mesh SDF unit tests.
- **SC-006**: Performance improvement scales appropriately with mesh complexity (larger meshes see proportionally larger absolute improvements).

## Assumptions

- The existing BVH construction quality is adequate; this feature focuses on query performance, not build-time optimization.
- Users have GPUs with at least OpenCL 1.2 support; no OpenCL 2.x features are assumed.
- The pseudo-normal sign determination algorithm remains unchanged; only its implementation efficiency is targeted.
- Performance profiling will be conducted on representative hardware (mid-range consumer GPU).

## Out of Scope

- BVH construction algorithm changes (covered separately if needed)
- Alternative spatial data structures (octrees, k-d trees)
- CPU-side mesh SDF evaluation
- Changes to the mesh import/loading pipeline
- Multi-GPU distribution of queries
