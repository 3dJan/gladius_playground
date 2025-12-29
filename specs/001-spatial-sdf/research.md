# Research: Spatial Tree Mesh SDF

**Feature**: 001-spatial-sdf  
**Date**: 2025-12-29  
**Status**: Complete

## Research Tasks

### 1. BVH Construction Strategy for Triangle Meshes

**Question**: What BVH construction algorithm should we use for triangle meshes?

**Decision**: Surface Area Heuristic (SAH) with binned splitting, following the existing `BeamBVHBuilder` pattern.

**Rationale**:
- SAH is the industry standard for ray tracing and closest-point queries
- The codebase already has a working SAH implementation in `BeamBVH.cpp`
- Binned splitting (sampling 32 split positions per axis) provides good quality without O(n²) cost
- Construction is CPU-side, so complexity is acceptable for build-once-query-many use case

**Alternatives Considered**:
- **Object Median Split**: Simpler but produces lower quality trees (rejected: SAH is already implemented)
- **LBVH (Linear BVH)**: Faster construction via Morton codes, but requires GPU construction (rejected: overkill for our use case; we want simple CPU build)
- **Octree**: Good for uniform distributions but BVH adapts better to arbitrary mesh distributions

**References**:
- Existing code: `gladius/src/BeamBVH.cpp`
- "Real-Time Collision Detection" by Ericson (already referenced in `sdf.cl`)

---

### 2. Closest Point on Triangle Algorithm

**Question**: How should we compute the closest point on a triangle to an arbitrary 3D point?

**Decision**: Use the existing `sqTriangle()` implementation in `sdf.cl` (Voronoi region method).

**Rationale**:
- Already implemented and tested in the codebase
- Based on Ericson's "Real-Time Collision Detection" algorithm
- Handles all cases: vertex, edge, and face regions
- Returns squared distance (avoid sqrt until final result)

**Implementation Note**: The current `sqTriangle()` only returns distance. We need to extend it to also return:
1. The closest point coordinates
2. The feature type (vertex/edge/face) for pseudo-normal computation
3. Barycentric coordinates for normal interpolation

**Alternatives Considered**:
- **Projection + clamping**: Less robust at corners and edges
- **Iterative methods**: Slower and unnecessary for triangles

---

### 3. Sign Determination: Weighted Pseudo-Normal Method

**Question**: How should we determine the sign of the distance for closed meshes?

**Decision**: Weighted pseudo-normal method using angle-weighted vertex normals.

**Rationale**:
- Robust for watertight meshes with consistent winding
- Works correctly even at mesh corners and edges where face normals are discontinuous
- Standard approach in mesh SDF literature (Bærentzen & Aanæs, 2005)

**Algorithm**:
1. Find closest point on mesh and determine feature type (vertex/edge/face)
2. **Face**: Use face normal directly
3. **Edge**: Interpolate vertex normals at edge endpoints using barycentric parameter `t`:
   ```c
   pseudoNormal = normalize((1-t) * vertexNormals[v0] + t * vertexNormals[v1]);
   ```
   This avoids needing edge-to-face adjacency data and works for typical meshes.
4. **Vertex**: Use precomputed angle-weighted normal directly (sum of face normals weighted by corner angles)
5. Compute `sign = dot(queryPoint - closestPoint, pseudoNormal) < 0 ? -1 : +1`
6. **Robustness**: If `length(pseudoNormal) < EPSILON`, fall back to closest face normal to handle degenerate cases (sharp creases > 90°, non-manifold edges)

**Data Requirements**:
- Per-vertex: Angle-weighted normal (sum of face normals weighted by corner angles)
- Per-triangle: Face normal, vertex indices
- **Note**: We can compute angle-weighted vertex normals during BVH build (one-time cost)

**Robustness Notes**:
- Sharp creases (>90°) can produce near-zero pseudo-normals; fall back to face normal
- Non-manifold edges have undefined pseudo-normals; use unsigned distance mode
- Edge interpolation using vertex normals is simpler than tracking edge adjacency and works correctly for manifold meshes

**Alternatives Considered**:
- **Ray casting (parity)**: Requires watertight mesh, O(log n) per query for ray-mesh intersection, more complex
- **Winding number**: Robust but expensive (O(n) per query without spatial structure)
- **Face normal only**: Fails at edges and corners (sign discontinuities)

**References**:
- Bærentzen & Aanæs, "Signed Distance Computation Using the Angle Weighted Pseudonormal" (IEEE TVCG, 2005)

---

### 4. OpenCL Kernel Design for BVH Traversal

**Question**: How should we implement BVH traversal in OpenCL for closest-point queries?

**Decision**: Stackless traversal using short-stack with rope/skip pointers, following patterns from existing `meshNode()` in `sdf.cl`.

**Rationale**:
- The codebase already uses stack-based traversal in `meshNode()` and `beamLatticeBVH()` functions
- Short stack (32-64 entries) is sufficient for balanced BVHs up to 2^32 primitives
- No OpenCL 2.x features required (no recursion, no dynamic allocation)

**Algorithm** (adapted from existing code):
```c
float closestDistanceBVH(float3 pos, int rootIndex, PAYLOAD_ARGS) {
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = rootIndex;
    
    float minSqDist = FLT_MAX;
    
    while (stackPtr > 0) {
        int nodeIdx = stack[--stackPtr];
        BVHNode node = nodes[nodeIdx];
        
        // Early exit if bounding box is farther than current best
        float boxDist = distanceToBBox(pos, node.bbox);
        if (boxDist * boxDist >= minSqDist) continue;
        
        if (node.isLeaf) {
            // Test all triangles in leaf
            for (int i = node.primStart; i < node.primStart + node.primCount; i++) {
                float sqDist = sqTriangleWithClosestPoint(pos, triangles[i], ...);
                if (sqDist < minSqDist) {
                    minSqDist = sqDist;
                    // Store closest point info for sign computation
                }
            }
        } else {
            // Push children (closer child last for better pruning)
            stack[stackPtr++] = node.rightChild;
            stack[stackPtr++] = node.leftChild;
        }
    }
    
    return sqrt(minSqDist) * sign;
}
```

**Memory Layout**: Nodes, triangles, and vertex normals stored in contiguous `PrimitiveBuffer` segments (same pattern as `VdbResource`).

---

### 5. Integration with Existing Node System

**Question**: How should we integrate with the existing `SignedDistanceToMesh` node?

**Decision**: Create `SpatialMeshResource` that provides the same interface as `VdbResource`, with a configuration option to select the backend.

**Rationale**:
- Minimal changes to node graph code
- Users can fall back to VDB path if needed
- Same resource loading pattern (`ResourceManager::addResource()`)

**Integration Points**:
1. `ResourceManager` → Add `addResource(ResourceKey, SpatialMeshData&&)` overload
2. `Importer3mf` → When loading mesh for SDF, create `SpatialMeshResource` instead of building VDB
3. `ProgramManager` → Add capability flag for spatial mesh SDF (no NanoVDB dependency)
4. `DerivedNodes.h` → Update `SignedDistanceToMesh` node to use new primitive type

**Primitive Type Addition** (in `types.h`):
```cpp
enum PrimitiveType {
    // ... existing types ...
    SDF_SPATIAL_MESH_ROOT = 20,    // Root metadata for spatial mesh SDF
    SDF_SPATIAL_MESH_NODES = 21,   // BVH node array
    SDF_SPATIAL_MESH_TRIS = 22,    // Triangle data
    SDF_SPATIAL_MESH_NORMALS = 23, // Weighted vertex normals for sign
};
```

---

### 6. Memory Layout for GPU Access

**Question**: How should we structure the data for efficient GPU access?

**Decision**: Interleaved node structure with triangle data and normals in separate arrays.

**Layout**:
```
PrimitiveBuffer segment for one mesh:

[Header: 1 PrimitiveMeta]
  - primitiveType = SDF_SPATIAL_MESH_ROOT
  - start = offset to node array
  - end = offset to end of all data
  
[Nodes: N × 48 bytes each]
  - float4 bboxMin (16 bytes)
  - float4 bboxMax (16 bytes)
  - int4 (leftChild, rightChild, primStart, primCount) (16 bytes)

[Triangles: T × 48 bytes each]
  - float3 v0, float3 v1, float3 v2
  - int3 vertexIndices (for normal lookup)
  
[Vertex Normals: V × 16 bytes each]
  - float3 weightedNormal (angle-weighted sum)
  - float padding
```

**Memory Estimate** (100K triangles, ~33K vertices):
- Nodes: ~200K nodes × 32B = 6.4 MB (generous estimate)
- Triangles: 100K × 48B = 4.8 MB
- Normals: 33K × 16B = 0.5 MB
- **Total**: ~12 MB (vs ~4.8 MB raw triangles = 2.5x overhead) ✅ Under 3x target

---

## Summary

| Topic | Decision |
|-------|----------|
| BVH Construction | SAH with binned splitting (reuse `BeamBVHBuilder` pattern) |
| Closest Point | Extend existing `sqTriangle()` to return point and feature type |
| Sign Determination | Weighted pseudo-normal (angle-weighted vertex normals) |
| OpenCL Traversal | Stack-based BVH traversal (existing pattern from `meshNode()`) |
| Integration | New `SpatialMeshResource` class, new primitive types in `types.h` |
| Memory Layout | Separate arrays for nodes, triangles, normals; ~2.5x overhead |

All research items resolved. Ready for Phase 1: Design & Contracts.
