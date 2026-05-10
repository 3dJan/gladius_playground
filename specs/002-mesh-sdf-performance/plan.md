# Implementation Plan: Mesh SDF Performance Improvements

**Feature**: 002-mesh-sdf-performance  
**Created**: 2025-12-30  
**Status**: Draft  
**Spec**: [spec.md](spec.md)

## Technical Context

| Aspect | Current State | Notes |
|--------|---------------|-------|
| **BVH Structure** | 48-byte nodes (`MeshBVHNodeGPU`) | float4 bbox × 2 + 4 ints |
| **Triangle Data** | 48-byte triangles (`MeshTriangleGPU`) | 3 × float4 vertices |
| **Vertex Normals** | 16-byte per normal | float4 (xyz used) |
| **Stack Size** | Fixed 64 entries | Sufficient for balanced trees |
| **Traversal** | Depth-first, no ordering | Children pushed in fixed order |
| **Memory Access** | Direct global reads | No caching mechanism |
| **Sign Computation** | Full pseudo-normal on every query | Including face/edge/vertex branches |

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Memory vs Speed** | Sacrifice memory for speed | User preference - faster rendering is priority |
| **Focus** | Rendering performance | Will also improve export speed |
| **Benchmark File** | `testdata/SphereInACageSimplifiedMesh.3mf` | Simple mesh for reproducible benchmarking |
| **API Changes** | Acceptable | Enables early termination threshold parameter |

## Constitution Check

Per project constitution guidelines:
- ✅ Correctness first (FR-001: results must match within FP tolerance)
- ✅ OpenCL 1.2 compatibility required (FR-005)
- ✅ No dynamic allocation on GPU
- ✅ Must work with existing `PrimitiveBuffer` layout

## Available Optimization Approaches

### Option A: Ordered Child Traversal (Front-to-Back)

**Concept**: When traversing internal BVH nodes, visit the child closer to the query point first. This finds good candidates faster, enabling more aggressive pruning of distant subtrees.

**From NanoVDB**: The `pnanovdb_readaccessor` caches the path to recently-accessed nodes, exploiting spatial locality in ray-marching where consecutive queries are often nearby.

**Adaptation for Mesh BVH**:
```c
// Current: fixed order, closer child may be visited second
stack[stackPtr++] = node.rightChild;
stack[stackPtr++] = node.leftChild;

// Proposed: visit closer child first for better pruning
float leftDist = sqDistanceToAABB(pos, leftNode.bboxMin, leftNode.bboxMax);
float rightDist = sqDistanceToAABB(pos, rightNode.bboxMin, rightNode.bboxMax);
if (leftDist < rightDist) {
    stack[stackPtr++] = node.rightChild;  // Far child first (processed last)
    stack[stackPtr++] = node.leftChild;   // Near child last (processed first)
} else {
    stack[stackPtr++] = node.leftChild;
    stack[stackPtr++] = node.rightChild;
}
```

**Pros**: Simple implementation, better pruning, no memory overhead  
**Cons**: Extra AABB distance computations for child nodes  
**Expected Gain**: 15-30% for large meshes (more nodes pruned)  
**Complexity**: Low

---

### Option B: Deferred Sign Computation

**Concept**: The current implementation computes the full pseudo-normal (face/edge/vertex) for every closest triangle candidate. Instead, defer sign computation until after finding the true closest point.

**Current Pattern** (mesh_sdf.cl):
```c
// Inside BVH leaf: compute closest point AND track all data for sign
if (sqDist < minSqDist) {
    minSqDist = sqDist;
    bestResult = result;  // Full ClosestPointResult: 9 fields
    bestTriIdx = triIdx;
}
// After traversal: compute sign using bestResult
```

**Proposed**:
```c
// Inside BVH leaf: only track minimal data for distance
if (sqDist < minSqDist) {
    minSqDist = sqDist;
    bestTriIdx = triIdx;
    bestBaryU = s;  // Only store if needed for sign
    bestBaryV = t;
}
// After traversal: recompute closest point details only for winner
// (single triangle test is cheap compared to traversal)
```

**Pros**: Reduces register pressure, fewer memory writes per candidate  
**Cons**: Must re-test winning triangle to get feature type  
**Expected Gain**: 10-20% (reduced register pressure improves occupancy)  
**Complexity**: Low

---

### Option C: Vectorized Data Loads

**Concept**: Use OpenCL vector types (`float4`, `vload4`) for coalesced memory access, matching patterns from `evaluateBeamLatticeBVH`.

**Current Pattern** (mesh_sdf.cl):
```c
struct MeshTriangleGPU tri = triangles[triIdx];  // Full 48-byte struct load
float3 v0 = tri.v0.xyz;
float3 v1 = tri.v1.xyz;
float3 v2 = tri.v2.xyz;
```

**Proposed**:
```c
// Direct vectorized loads from data array (like beam lattice BVH)
int triDataStart = trianglesOffset + triIdx * 12;  // 12 floats per triangle
float4 v0_4 = vload4(0, &data[triDataStart]);
float4 v1_4 = vload4(0, &data[triDataStart + 4]);
float4 v2_4 = vload4(0, &data[triDataStart + 8]);
float3 v0 = v0_4.xyz;
float3 v1 = v1_4.xyz;
float3 v2 = v2_4.xyz;
```

**Pros**: Better memory coalescing on many GPUs, consistent with existing patterns  
**Cons**: Requires data layout verification, may not help all GPU architectures  
**Expected Gain**: 5-15% (memory-bandwidth limited workloads)  
**Complexity**: Low

---

### Option D: Distance-Ordered Priority Queue

**Concept**: Replace stack with a priority queue (min-heap) sorted by AABB distance. Process nodes in distance order to find the closest primitive faster.

**From NanoVDB**: The hierarchical accessor maintains a path to the current voxel; neighbor access reuses parent nodes when possible.

**Adaptation**:
```c
// Instead of stack: maintain sorted list of candidate nodes by distance
struct NodeCandidate { int nodeIdx; float sqDist; };
NodeCandidate queue[64];
int queueSize = 0;

// Insert new candidates in sorted order (binary insertion)
// Always process minimum-distance node first
```

**Pros**: Optimal traversal order, finds true minimum faster  
**Cons**: Higher per-node overhead, complex insertion logic  
**Expected Gain**: 20-40% for very large meshes (highly non-uniform)  
**Complexity**: Medium-High

---

### Option E: Precomputed Face Normals

**Concept**: Store precomputed face normals with triangles to avoid cross-product in sign computation for face-closest cases (~60% of cases).

**Current**:
```c
if (result->featureType == 0) {  // Face
    float3 e0 = v1 - v0;
    float3 e1 = v2 - v0;
    pseudoNormal = cross(e0, e1);  // 6 multiplies, 3 subtracts
}
```

**Proposed**:
```c
// Triangle data extended with precomputed normal
struct MeshTriangleGPU {
    float4 v0, v1, v2;
    float4 normal;  // NEW: precomputed (adds 16 bytes = 64 bytes total)
};

if (result->featureType == 0) {
    pseudoNormal = tri.normal.xyz;  // Single memory read
}
```

**Pros**: Eliminates cross product for majority of cases  
**Cons**: 33% memory increase for triangle data  
**Expected Gain**: 5-10%  
**Complexity**: Low  
**Status**: ✅ APPROVED (memory sacrifice acceptable) (but memory cost may be prohibitive)

---

### Option F: Early Termination Threshold

**Concept**: For rendering, exact minimum distance is often unnecessary. Stop traversal when distance is below a threshold (e.g., pixel size × constant).

**From Beam Lattice** (sdf.cl:1395):
```c
// Early exit if we're very close (GPU-friendly condition)
if (dist < voxelSize * 0.1f) {
    break; // Found very close primitive, no need to check others
}
```

**Adaptation for Mesh SDF**:
```c
// Optional early termination for rendering workloads
if (minSqDist < earlyExitThresholdSq) {
    break;  // Close enough for raymarching
}
```

**Pros**: Major speedup for raymarching (many queries don't need exact result)  
**Cons**: Requires threshold parameter, not suitable for all use cases (mesh export)  
**Expected Gain**: 30-50% for rendering workloads  
**Complexity**: Low (but requires API change for optional parameter)

---

### Option G: Voxel Acceleration Grid with Nearest Triangle Indices

**Concept**: Build a coarse voxel grid that stores the index of the nearest triangle(s) per voxel. For queries far from the surface, return an approximate distance. Near the surface, fall back to exact BVH traversal.

**From Beam Lattice** (sdf.cl:1185-1220):
```c
// Existing voxel grid lookup pattern
uint primitiveIndexFromVoxelGrid(float3 pos, int voxelGridIndex, PAYLOAD_ARGS) {
    float3 origin = (float3)(data[dataStart], data[dataStart + 1], data[dataStart + 2]);
    int3 dimensions = (int3)((int)data[dataStart + 3], ...);
    float voxelSize = data[dataStart + 6];
    // ... O(1) lookup into voxel grid
}
```

**Adaptation for Mesh SDF**:

```c
/// Voxel grid for mesh SDF acceleration
/// Each voxel stores:
///   - nearestTriIndex: index of closest triangle to voxel center
///   - approxSignedDist: signed distance at voxel center (precomputed)
///
/// Memory layout (per voxel): 2 floats = 8 bytes
///   [0]: (float)nearestTriIndex  
///   [1]: approxSignedDist

float spatialMeshSDF_Accelerated(float3 pos, 
                                  int voxelGridIndex,
                                  int nodesOffset, int trianglesOffset, ...)
{
    // Step 1: O(1) voxel lookup
    float2 voxelData = lookupMeshVoxel(pos, voxelGridIndex, PASS_PAYLOAD_ARGS);
    int nearestTriIdx = (int)voxelData.x;
    float approxDist = voxelData.y;
    
    // Step 2: If far from surface, return approximate distance
    float threshold = voxelSize * 1.5f;  // ~1.5 voxel diagonal
    if (fabs(approxDist) > threshold) {
        // Far from surface: compute exact distance to cached triangle only
        float exactDist = distanceToTriangle(pos, nearestTriIdx, ...);
        return exactDist;  // Sign from approxDist
    }
    
    // Step 3: Near surface - fall back to full BVH traversal for accuracy
    return spatialMeshSDF(pos, nodesOffset, trianglesOffset, ...);
}
```

**Voxel Grid Construction** (CPU-side, during mesh import):
```cpp
void buildMeshVoxelGrid(SpatialMeshData const& meshData, 
                        BoundingBox const& bbox,
                        float voxelSize) 
{
    // 1. Compute grid dimensions from bbox and voxelSize
    int3 dims = ceil((bbox.max - bbox.min) / voxelSize);
    
    // 2. For each voxel center, find nearest triangle using BVH
    for (int z = 0; z < dims.z; ++z) {
        for (int y = 0; y < dims.y; ++y) {
            for (int x = 0; x < dims.x; ++x) {
                float3 center = bbox.min + (float3(x, y, z) + 0.5f) * voxelSize;
                auto [triIdx, signedDist] = queryNearestTriangle(center, meshData);
                voxelGrid[z][y][x] = {triIdx, signedDist};
            }
        }
    }
}
```

**Resolution Strategy**:
- Coarse grid: voxelSize = bbox diagonal / 32 (for 32³ = 32K voxels)
- Medium grid: voxelSize = bbox diagonal / 64 (for 64³ = 262K voxels)
- Memory: 8 bytes/voxel → 256KB for 32³, 2MB for 64³

**Query Behavior**:
| Distance to Surface | Action | Cost |
|---------------------|--------|------|
| > 1.5 × voxelSize | Use cached triangle only | O(1) lookup + 1 triangle test |
| ≤ 1.5 × voxelSize | Full BVH traversal | O(log n) |

**Pros**: 
- Massive speedup for queries far from surface (most raymarching samples)
- O(1) for ~80% of queries in typical rendering
- Existing voxel grid infrastructure can be reused

**Cons**: 
- Memory overhead (256KB - 2MB depending on resolution)
- CPU build time during mesh import
- Threshold tuning needed

**Expected Gain**: 50-80% for rendering workloads  
**Complexity**: Medium  
**Status**: ✅ PROMOTED TO PHASE 2 (high impact for rendering focus)

---

### Option H: Simplified Distance-Only Mode

**Concept**: Provide an optimized unsigned-distance function that skips all sign computation overhead for use cases that don't need sign.

**Current**: `spatialMeshUnsignedDistance` exists but shares much code with signed version.

**Proposed**: Fully optimized version with:
- No vertex index tracking
- No feature type determination
- Simplified `sqTriangle` without closest point details

**Pros**: Faster for CSG shell operations, offset computations  
**Cons**: Code duplication vs. maintainability tradeoff  
**Expected Gain**: 15-25% for unsigned queries  
**Complexity**: Low

---

## Recommended Implementation Order

Based on effort/impact ratio and rendering focus.

> **Note**: P1/P2/P3 are priority tiers within each phase, not phase numbers.
> P1 = implement first, P2 = implement after P1 complete, P3 = implement last or defer.

| Priority | Option | Expected Gain | Effort | Status |
|----------|--------|---------------|--------|--------|
| **P1** | A: Ordered Traversal | 15-30% | Low | ✅ Implement |
| **P1** | B: Deferred Sign | 10-20% | Low | ✅ Implement |
| **P1** | F: Early Termination | 30-50% | Low | ✅ Implement (rendering focus) |
| **P2** | G: Voxel Acceleration Grid | 50-80% | Medium | ✅ Implement (high impact) |
| **P2** | C: Vectorized Loads | 5-15% | Low | ✅ Implement |
| **P2** | E: Face Normals | 5-10% | Low | ✅ Implement (memory OK) |
| **P3** | H: Distance-Only Mode | 15-25% | Low | ✅ Implement |
| **P3** | D: Priority Queue | 20-40% | Med-High | Evaluate after P1/P2 |

## Phase 1 Implementation Tasks

### Task 1.1: Ordered Child Traversal (Option A)
**Files**: `mesh_sdf.cl`  
**Changes**:
1. Compute AABB distance to both children before pushing to stack
2. Push far child first, near child last (LIFO order = near processed first)
3. Verify: run existing tests, measure improvement

### Task 1.2: Deferred Sign Computation (Option B)
**Files**: `mesh_sdf.cl`  
**Changes**:
1. Reduce `ClosestPointResult` tracking to minimal fields during traversal
2. After traversal: re-run `sqTriangleWithClosestPoint` on winning triangle only
3. Compute pseudo-normal only for final closest point

### Task 1.3: Add Performance Benchmarks
**Files**: New test file or addition to existing spatial mesh tests  
**Benchmark Meshes**: 
- Primary: `testdata/SphereInACageSimplifiedMesh.3mf` (simple, reproducible)
- Scale test: Consider additional meshes (100, 10K, 100K, 1M triangles) for FR-004 validation  
**Purpose**: Measure baseline and track improvements across all optimizations

## Phase 2 Implementation Tasks

### Task 2.1: Voxel Acceleration Grid (Option G) - HIGH PRIORITY
**Files**: New `MeshVoxelGrid.h/.cpp`, `mesh_sdf.cl`, `SpatialMeshResource.h/.cpp`  
**Changes**:

1. **Data Structure** (GPU buffer):
   ```cpp
   // GPU memory layout per voxel (8 bytes = 2 floats)
   struct MeshVoxelData {
       float nearestTriIndex;   // Stored as float for GPU compatibility
       float approxSignedDist;  // Signed distance at voxel center
   };
   
   // Header (uploaded once, 10 floats)
   struct MeshVoxelGridHeader {
       float3 origin;           // Grid origin (bbox min)
       int3 dimensions;         // Grid dimensions (as floats)
       float voxelSize;         // Size of each voxel
       float invVoxelSize;      // 1.0 / voxelSize (precomputed)
       float threshold;         // Distance threshold for BVH fallback
   };
   ```

2. **GPU Build Kernel** (`mesh_sdf.cl`):
   ```c
   /// Build voxel acceleration grid on GPU
   /// Launch with global size = dimensions.x * dimensions.y * dimensions.z
   __kernel void buildMeshVoxelGrid(
       __global float* voxelData,        // Output: 2 floats per voxel
       __global float const* header,     // Grid header (origin, dims, voxelSize)
       int nodesOffset,                  // BVH data offsets
       int trianglesOffset,
       int normalsOffset,
       int indicesOffset,
       int nodeCount,
       int triCount,
       int vertexNormalCount,
       PAYLOAD_ARGS)
   {
       int voxelIdx = get_global_id(0);
       
       // Compute voxel center from linear index
       int3 dims = (int3)(header[3], header[4], header[5]);
       float3 origin = (float3)(header[0], header[1], header[2]);
       float voxelSize = header[6];
       
       int z = voxelIdx / (dims.x * dims.y);
       int y = (voxelIdx / dims.x) % dims.y;
       int x = voxelIdx % dims.x;
       
       float3 center = origin + (convert_float3((int3)(x, y, z)) + 0.5f) * voxelSize;
       
       // Query signed distance using existing BVH traversal
       float signedDist = spatialMeshSDF(center, nodesOffset, trianglesOffset, 
                                          normalsOffset, indicesOffset,
                                          nodeCount, triCount, vertexNormalCount, data);
       
       // Store results (could also store nearest triangle index if needed)
       voxelData[voxelIdx * 2 + 0] = 0.0f;  // Reserved for triangle index
       voxelData[voxelIdx * 2 + 1] = signedDist;
   }
   ```

3. **Build Process** (host-side):
   ```cpp
   void SpatialMeshResource::buildVoxelGridGPU(ComputeContext& ctx) {
       // 1. Allocate GPU buffer for voxel grid
       int numVoxels = dims.x * dims.y * dims.z;
       m_voxelBuffer = ctx.createBuffer<float>(numVoxels * 2);
       
       // 2. Upload header
       uploadVoxelHeader(ctx, origin, dims, voxelSize);
       
       // 3. Launch build kernel (fully parallel - one thread per voxel)
       ctx.enqueueKernel("buildMeshVoxelGrid", {numVoxels}, ...);
       
       // No CPU readback needed - data stays on GPU!
   }
   ```

4. **GPU Query with Position-Aware 2×2×2 Stencil** (`mesh_sdf.cl`):
   ```c
   /// Query mesh SDF using voxel acceleration with smart 2x2x2 stencil
   /// Stencil selection based on position within voxel - only 8 lookups instead of 27!
   float spatialMeshSDF_VoxelAccelerated(float3 pos, 
                                          int voxelGridOffset,
                                          int trianglesOffset,
                                          int normalsOffset, ...)
   {
       // O(1) voxel lookup
       float3 localPos = (pos - origin) * invVoxelSize;
       int3 baseCoord = convert_int3(floor(localPos));
       
       // Bounds check (need room for 2x2x2 stencil)
       if (any(baseCoord < 0) || any(baseCoord >= dims - 1)) {
           return spatialMeshSDF(pos, ...);  // Near boundary - full BVH
       }
       
       // Compute fractional position within voxel [0, 1)
       float3 frac = localPos - floor(localPos);
       
       // Select 2x2x2 stencil based on position within voxel
       // If frac >= 0.5: we're closer to the "plus" neighbor on that axis
       // If frac < 0.5: we're closer to the "minus" neighbor (current cell)
       int3 stencilBase = baseCoord + (int3)(
           (frac.x >= 0.5f) ? 0 : -1,
           (frac.y >= 0.5f) ? 0 : -1,
           (frac.z >= 0.5f) ? 0 : -1
       );
       
       // Clamp to valid range
       stencilBase = clamp(stencilBase, (int3)(0, 0, 0), dims - 2);
       
       // Gather triangles from 2x2x2 stencil (only 8 voxels!)
       float minSqDist = FLT_MAX;
       int bestTriIdx = -1;
       int testedTriangles[8];
       int numTested = 0;
       
       // Unrolled 2x2x2 loop for maximum performance
       #pragma unroll
       for (int dz = 0; dz < 2; ++dz) {
           #pragma unroll
           for (int dy = 0; dy < 2; ++dy) {
               #pragma unroll
               for (int dx = 0; dx < 2; ++dx) {
                   int3 coord = stencilBase + (int3)(dx, dy, dz);
                   int voxelIdx = coord.z * dims.y * dims.x + 
                                  coord.y * dims.x + coord.x;
                   
                   int triIdx = (int)voxelData[voxelIdx * 2 + 0];
                   if (triIdx < 0) continue;
                   
                   // Skip if already tested this triangle
                   bool alreadyTested = false;
                   for (int j = 0; j < numTested; ++j) {
                       if (testedTriangles[j] == triIdx) {
                           alreadyTested = true;
                           break;
                       }
                   }
                   if (alreadyTested) continue;
                   
                   // Test this triangle
                   float sqDist = sqDistanceToTriangle(pos, triIdx, trianglesOffset, data);
                   if (sqDist < minSqDist) {
                       minSqDist = sqDist;
                       bestTriIdx = triIdx;
                   }
                   
                   testedTriangles[numTested++] = triIdx;
               }
           }
       }
       
       // Compute sign using pseudo-normal for best triangle
       if (bestTriIdx < 0) {
           return spatialMeshSDF(pos, ...);  // Fallback to BVH
       }
       
       float sign = computeSignForTriangle(pos, bestTriIdx, ...);
       return sign * sqrt(minSqDist);
   }
   ```

   **Position-Aware 2×2×2 Stencil Selection**:
   ```
   Example: Query point at frac = (0.7, 0.3, 0.8) within voxel (5, 5, 5)
   
   - x: frac.x=0.7 >= 0.5 → include voxels 5 and 6 in x
   - y: frac.y=0.3 <  0.5 → include voxels 4 and 5 in y
   - z: frac.z=0.8 >= 0.5 → include voxels 5 and 6 in z
   
   Stencil base = (5, 4, 5)
   2×2×2 covers: x∈[5,6], y∈[4,5], z∈[5,6]
   
        Query point P is always inside the 2×2×2 stencil!
   
   ┌─────────┬─────────┐
   │ (5,5,6) │ (6,5,6) │  ← z=6 layer
   ├─────────┼─────────┤
   │ (5,4,6) │ (6,4,6) │
   └─────────┴─────────┘
   
   ┌─────────┬─────────┐
   │ (5,5,5) │ (6,5,5) │  ← z=5 layer (P is here)
   ├────•────┼─────────┤     • = query point
   │ (5,4,5) │ (6,4,5) │
   └─────────┴─────────┘
   ```

   **Why 2×2×2 is Sufficient**:
   - The closest point must be within sqrt(3) × voxelSize of P
   - By selecting stencil based on P's octant, we always include P's position
   - Maximum distance from P to any stencil corner = sqrt(3) × voxelSize ✓
   
   **Performance**: 8 voxel reads instead of 27 = **3.4× fewer memory accesses**

5. **Integration**:
   - Trigger build after BVH upload to GPU
   - No CPU-GPU data transfer for voxel grid (built and used entirely on GPU)
   - Add kernel to `CLProgram` compilation

**Benefits of GPU Build**:
- 32³ voxels = 32K parallel threads (saturates GPU)
- No CPU-GPU round trip for voxel data
- Build time: ~10-50ms for typical meshes (vs. seconds on CPU)
- Can rebuild dynamically if mesh transforms

**Resolution**: Start with 32³ grid (256KB), make configurable

### Task 2.2: Early Termination Mode (Option F)
**Files**: `mesh_sdf.cl`, `SpatialMeshResource.h`  
**Changes**:
1. Add `earlyExitDistanceSq` parameter to `spatialMeshSDF` function
2. Add early termination check in leaf processing loop: `if (minSqDist < earlyExitDistanceSq) break;`
3. Default to 0.0 (disabled) for backward compatibility
4. Document typical values for rendering (e.g., pixel size² × 4)

### Task 2.3: Precomputed Face Normals (Option E)
**Files**: `MeshBVH.h`, `MeshBVH.cpp`, `mesh_sdf.cl`  
**Changes**:
1. Extend `MeshTriangleGPU` to 64 bytes with `float4 normal` field
2. Compute and store face normal during BVH build
3. Use stored normal in `computePseudoNormal` for face-closest cases
4. Update GPU memory layout offsets

### Task 2.4: Vectorized Memory Access (Option C)
**Files**: `mesh_sdf.cl`  
**Changes**:
1. Use `vload4` for BVH node data access (bbox min, bbox max)
2. Use `vload4` for triangle vertex access
3. Benchmark memory-bound workloads

### Task 2.5: Optimized Unsigned Distance (Option H)
**Files**: `mesh_sdf.cl`  
**Changes**:
1. Create streamlined `spatialMeshUnsignedDistanceFast`
2. Remove all feature-type and index tracking
3. Use minimal `sqTriangle` variant (distance only, no closest point details)

## Phase 3: Priority Queue (Future)

### Task 3.1: Distance-Ordered Priority Queue (Option D)
**Files**: `mesh_sdf.cl`  
**Changes**:
1. Replace stack with sorted candidate list by AABB distance
2. Always process minimum-distance node first
3. Evaluate performance vs. complexity tradeoff
**Expected Gain**: 20-40% for very large, non-uniform meshes

## Success Validation

**Benchmark Setup**: `SphereInACageSimplifiedMesh.3mf`  
**Measurement**: Time N random SDF queries, average per-query time

- [ ] All existing `SpatialMesh*` tests pass
- [ ] Rendering frame rate improves measurably on test scenes
- [ ] Export operations complete faster  
- [ ] Performance scales with mesh size (larger meshes see larger absolute gains)
- [ ] Cumulative improvement target: **≥40%** reduction in average query time
