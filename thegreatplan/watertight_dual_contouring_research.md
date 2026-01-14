# Watertight Surface Extraction Research for Manifold Dual Contouring

## Executive Summary

Research into watertight mesh generation with Dual Contouring reveals that **the primary cause of open edges is missing neighbor cells**. When a surface-intersecting cell has an edge that needs 4 cells to form a quad, but one or more of those neighbors don't exist (because they're entirely outside the surface), that edge becomes a boundary edge, creating a hole.

Your implementation already has most of the required infrastructure, but the **halo node generation is incomplete**. The current implementation creates halo nodes for the 26 face/edge/corner neighbors, but doesn't handle the **cascading problem**: halo nodes themselves need neighbors to emit quads.

## Current Implementation Status

### ✅ Working Components

1. **2:1 Octree Balancing** ([DualContouringOctree.cpp:873](gladius/src/DualContouringOctree.cpp#L873-L920))
   - Multi-pass balanced refinement ensures depth difference ≤ 1
   - Prevents T-junctions effectively
   - Used in CPU dual contouring

2. **Global Morton Code Indexing** ([GlobalMortonOctree.cpp](gladius/src/compute/GlobalMortonOctree.cpp))
   - Path-based Morton codes for hierarchical structure
   - Binary search for exact neighbor lookup
   - Currently DISABLED due to incomplete halo implementation

3. **Manifold DC Component Analysis** ([manifold_dual_contouring.cl](gladius/src/kernel/manifold_dual_contouring.cl))
   - Sign configuration analysis (8-bit corner mask + 12-bit edge mask)
   - Gradient discontinuity detection (0.3 threshold ≈ 72°)
   - Multiple vertices per cell (1-4 vertices)

4. **Edge Ownership Rules** ([manifold_dual_contouring.cl:1272](gladius/src/kernel/manifold_dual_contouring.cl#L1272))
   - Edges 0, 3, 8 owned by MIN corner cell
   - Edges 6, 5, 10 owned by MAX corner cell
   - Prevents duplicate quads
´
5. **Chunked Approach** ([ManifoldDualContouringGpu.cpp](gladius/src/compute/ManifoldDualContouringGpu.cpp))
   - Currently active and working
   - Produces ~1% boundary edges (acceptable for 3D printing)
   - Uses bridge triangles at chunk boundaries

### ⚠️ Partial Implementation

**Halo Node Generation** ([ManifoldDualContouringProgram.cpp:259](gladius/src/compute/ManifoldDualContouringProgram.cpp#L259-L380))
- Creates halo nodes for 26-neighborhood around surface cells
- Correctly merges edgeMasks when multiple surface cells contribute
- **Problem**: Doesn't handle the cascading requirement

### ❌ Missing Component

**Iterative/Cascading Halo Creation**
- Halo nodes need their own neighbors to emit quads
- Current implementation only creates one layer of halo nodes
- Test case [ManifoldDualContouring_tests.cpp:2828](gladius/tests/unittests/ManifoldDualContouring_tests.cpp#L2828) documents this issue

## Root Cause Analysis

### The Cascading Halo Problem

```
Example scenario:
1. Surface cell A at (x,y,z) has edgeMask with edge 0 set (X-aligned edge)
2. To emit quad for edge 0, A needs 4 cells: (x,y,z), (x,y-1,z), (x,y,z-1), (x,y-1,z-1)
3. Cell (x,y-1,z) doesn't exist → halo H created at (x,y-1,z)
4. Halo H inherits edgeMask bit 6 (corresponding edge from perspective of (x,y-1,z))
5. To emit quad for edge 6, H needs: (x,y-1,z), (x,y-1,z+1), (x,y-2,z), (x,y-2,z+1)
6. If (x,y-2,z) doesn't exist → H cannot emit quad → HOLE
```

This is confirmed by test analysis in [ManifoldDualContouring_tests.cpp:2931](gladius/tests/unittests/ManifoldDualContouring_tests.cpp#L2931-L2943).

### Why Global Morton Octree Fails

From [GlobalMortonOctree.cpp:21](gladius/src/compute/GlobalMortonOctree.cpp#L21-L26):

```cpp
// NOTE: This GlobalMortonOctree implementation is EXPERIMENTAL and DISABLED by default.
// Currently disabled because edge-to-cells mapping fails when neighbor cells don't
// intersect the surface (common at boundaries), causing non-manifold meshes.
// The GPU chunked approach (used when enableHierarchicalOctree=false) works correctly.
```

The issue is documented in [GlobalMortonOctree.cpp:1417](gladius/src/compute/GlobalMortonOctree.cpp#L1417-L1450): when looking up the 3 neighbor cells for an edge quad, if any don't exist in the Morton-to-node map, it becomes a boundary edge.

## Research: Guaranteed Watertightness Methods

### 1. Complete Halo Shell Approach (RECOMMENDED)

Create a **complete shell** of cells around all surface-intersecting cells, ensuring every edge has exactly 4 cells.

**Algorithm:**
```cpp
void completeHaloShell(std::vector<OctreeNode>& nodes, uint32_t maxCoord, uint8_t depth)
{
    std::unordered_set<uint64_t> existingMortons;
    std::unordered_set<uint64_t> needsHalo; // Morton codes that need halo neighbors
    
    // Phase 1: Identify all surface cells
    for (auto const& node : nodes)
    {
        existingMortons.insert(node.mortonCode);
        if (node.edgeMask != 0)
        {
            needsHalo.insert(node.mortonCode);
        }
    }
    
    // Phase 2: Iteratively create halo layers until convergence
    bool changed = true;
    size_t iteration = 0;
    constexpr size_t MAX_ITERATIONS = 10;
    
    while (changed && iteration < MAX_ITERATIONS)
    {
        changed = false;
        ++iteration;
        
        std::vector<OctreeNode> newHalos;
        std::unordered_set<uint64_t> nextNeedsHalo;
        
        for (uint64_t morton : needsHalo)
        {
            uint3 coords = decodeMorton3(morton);
            
            // For each of the 12 edges
            for (int edge = 0; edge < 12; ++edge)
            {
                // Get 4 cells that share this edge
                auto edgeCells = getEdgeNeighbors(coords, edge, depth);
                
                for (auto const& [nx, ny, nz] : edgeCells)
                {
                    // Bounds check
                    if (nx < 0 || nx > maxCoord || 
                        ny < 0 || ny > maxCoord || 
                        nz < 0 || nz > maxCoord)
                        continue;
                    
                    uint64_t neighborMorton = encodeMorton3(nx, ny, nz);
                    
                    if (existingMortons.find(neighborMorton) == existingMortons.end())
                    {
                        // Create halo node
                        OctreeNode halo;
                        halo.mortonCode = neighborMorton;
                        halo.depth = depth;
                        halo.edgeMask = computeHaloEdgeMask(morton, edge, coords, nx, ny, nz);
                        halo.internalMask = 0;
                        
                        newHalos.push_back(halo);
                        existingMortons.insert(neighborMorton);
                        
                        // If this halo has edges, it might need its own neighbors
                        if (halo.edgeMask != 0)
                        {
                            nextNeedsHalo.insert(neighborMorton);
                        }
                        
                        changed = true;
                    }
                }
            }
        }
        
        // Merge new halos into node list
        nodes.insert(nodes.end(), newHalos.begin(), newHalos.end());
        needsHalo = std::move(nextNeedsHalo);
    }
    
    std::cout << "Halo shell complete after " << iteration << " iterations" << std::endl;
}
```

**Key Edge Neighbor Lookup:**
```cpp
// For a cell at (cx, cy, cz), get 4 cells sharing a specific edge
std::array<std::tuple<int,int,int>, 4> getEdgeNeighbors(uint3 coords, int edge, uint8_t depth)
{
    uint32_t cx = coords.x, cy = coords.y, cz = coords.z;
    
    // Edge numbering (from manifold_dual_contouring.cl):
    // 0: X at y=0,z=0 | 1: Y at x=1,z=0 | 2: X at y=1,z=0 | 3: Y at x=0,z=0
    // 4: X at y=0,z=1 | 5: Y at x=1,z=1 | 6: X at y=1,z=1 | 7: Y at x=0,z=1
    // 8: Z at x=0,y=0 | 9: Z at x=1,y=0 | 10: Z at x=1,y=1 | 11: Z at x=0,y=1
    
    switch(edge)
    {
        case 0: // X-axis at min Y, min Z
            return {{{cx,cy,cz}, {cx,cy-1,cz}, {cx,cy,cz-1}, {cx,cy-1,cz-1}}};
        case 1: // Y-axis at max X, min Z
            return {{{cx,cy,cz}, {cx+1,cy,cz}, {cx,cy,cz-1}, {cx+1,cy,cz-1}}};
        case 2: // X-axis at max Y, min Z
            return {{{cx,cy,cz}, {cx,cy+1,cz}, {cx,cy,cz-1}, {cx,cy+1,cz-1}}};
        case 3: // Y-axis at min X, min Z
            return {{{cx,cy,cz}, {cx-1,cy,cz}, {cx,cy,cz-1}, {cx-1,cy,cz-1}}};
        case 4: // X-axis at min Y, max Z
            return {{{cx,cy,cz}, {cx,cy-1,cz}, {cx,cy,cz+1}, {cx,cy-1,cz+1}}};
        case 5: // Y-axis at max X, max Z
            return {{{cx,cy,cz}, {cx+1,cy,cz}, {cx,cy,cz+1}, {cx+1,cy,cz+1}}};
        case 6: // X-axis at max Y, max Z
            return {{{cx,cy,cz}, {cx,cy+1,cz}, {cx,cy,cz+1}, {cx,cy+1,cz+1}}};
        case 7: // Y-axis at min X, max Z
            return {{{cx,cy,cz}, {cx-1,cy,cz}, {cx,cy,cz+1}, {cx-1,cy,cz+1}}};
        case 8: // Z-axis at min X, min Y
            return {{{cx,cy,cz}, {cx-1,cy,cz}, {cx,cy-1,cz}, {cx-1,cy-1,cz}}};
        case 9: // Z-axis at max X, min Y
            return {{{cx,cy,cz}, {cx+1,cy,cz}, {cx,cy-1,cz}, {cx+1,cy-1,cz}}};
        case 10: // Z-axis at max X, max Y
            return {{{cx,cy,cz}, {cx+1,cy,cz}, {cx,cy+1,cz}, {cx+1,cy+1,cz}}};
        case 11: // Z-axis at min X, max Y
            return {{{cx,cy,cz}, {cx-1,cy,cz}, {cx,cy+1,cz}, {cx-1,cy+1,cz}}};
        default:
            return {{{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}}};
    }
}
```

### 2. Edge-Centric Approach (Alternative)

Instead of cell-centric halo creation, enumerate all surface-crossing edges and ensure 4 cells exist for each.

**Algorithm:**
```cpp
void ensureEdgeCompleteness(std::vector<OctreeNode>& nodes, uint32_t maxCoord, uint8_t depth)
{
    std::unordered_map<uint64_t, OctreeNode*> mortonToNode;
    
    for (auto& node : nodes)
    {
        mortonToNode[node.mortonCode] = &node;
    }
    
    // Collect all surface-crossing edges
    struct Edge
    {
        uint3 minCorner;  // Minimum corner of the edge's 2x2x1 cell cluster
        uint8_t axis;     // 0=X, 1=Y, 2=Z
    };
    
    std::set<std::tuple<uint32_t,uint32_t,uint32_t,uint8_t>> edgeSet;
    
    for (auto& node : nodes)
    {
        if (node.edgeMask == 0) continue;
        
        uint3 coords = decodeMorton3(node.mortonCode);
        
        for (int e = 0; e < 12; ++e)
        {
            if (!(node.edgeMask & (1 << e))) continue;
            
            // Compute canonical edge representation
            auto [minCorner, axis] = getCanonicalEdge(coords, e);
            edgeSet.insert({minCorner.x, minCorner.y, minCorner.z, axis});
        }
    }
    
    // For each edge, ensure all 4 cells exist
    for (auto const& [ex, ey, ez, axis] : edgeSet)
    {
        auto cells = getCellsForEdge({ex, ey, ez}, axis);
        
        for (auto const& [cx, cy, cz] : cells)
        {
            if (cx > maxCoord || cy > maxCoord || cz > maxCoord)
                continue;
            
            uint64_t morton = encodeMorton3(cx, cy, cz);
            
            if (mortonToNode.find(morton) == mortonToNode.end())
            {
                // Create halo node
                OctreeNode halo;
                halo.mortonCode = morton;
                halo.depth = depth;
                halo.edgeMask = 0; // Halo nodes don't emit quads
                halo.internalMask = 0;
                
                nodes.push_back(halo);
                mortonToNode[morton] = &nodes.back();
            }
        }
    }
}
```

### 3. Restricted Octree with Guaranteed Neighbors (Literature Approach)

Based on "Isosurface Stuffing" (Labelle & Shewchuk, 2007) and "Dual Marching Cubes" (Schaefer & Warren, 2004):

**Key Principles:**
1. **Complete boundary layer**: Always subdivide one layer beyond the surface
2. **Balanced octree**: 2:1 depth constraint (already implemented)
3. **Face-adjacent completeness**: Every intersecting cell must have all 6 face neighbors
4. **Edge-adjacent completeness**: For watertight DC, need all 12 edge neighbors

**Implementation in GlobalMortonOctree:**

The [balanceOctree](gladius/src/compute/GlobalMortonOctree.cpp#L500-L600) function implements this but only for **face-adjacent** neighbors. Extend it to **edge-adjacent**:

```cpp
void GlobalMortonOctree::balanceOctree()
{
    // ... existing face-adjacent logic ...
    
    // ADDITION: Ensure edge-adjacent neighbors for watertightness
    for (std::size_t nodeIdx : intersectingLeaves)
    {
        auto const& node = m_nodes[nodeIdx];
        
        std::uint32_t cx = 0U, cy = 0U, cz = 0U;
        decodePathMorton(node.mortonCode, node.depth, cx, cy, cz);
        
        auto const maxCoord = (1U << node.depth) - 1U;
        
        // 12 edge-adjacent neighbors (not including face-adjacent)
        std::array<std::tuple<int, int, int>, 12> const edgeOffsets = {{
            {1, 1, 0}, {1, -1, 0}, {-1, 1, 0}, {-1, -1, 0},  // XY edges
            {1, 0, 1}, {1, 0, -1}, {-1, 0, 1}, {-1, 0, -1},  // XZ edges
            {0, 1, 1}, {0, 1, -1}, {0, -1, 1}, {0, -1, -1}   // YZ edges
        }};
        
        for (auto const& [dx, dy, dz] : edgeOffsets)
        {
            auto const nx = static_cast<std::int32_t>(cx) + dx;
            auto const ny = static_cast<std::int32_t>(cy) + dy;
            auto const nz = static_cast<std::int32_t>(cz) + dz;
            
            if (nx < 0 || nx > static_cast<std::int32_t>(maxCoord) ||
                ny < 0 || ny > static_cast<std::int32_t>(maxCoord) ||
                nz < 0 || nz > static_cast<std::int32_t>(maxCoord))
            {
                continue;
            }
            
            std::uint64_t const neighborMorton = encodePathMorton(
                static_cast<std::uint32_t>(nx),
                static_cast<std::uint32_t>(ny),
                static_cast<std::uint32_t>(nz),
                node.depth);
            
            if (m_mortonToIndex.find(neighborMorton) == m_mortonToIndex.end())
            {
                createNodeAtCoordinates(
                    static_cast<std::uint32_t>(nx),
                    static_cast<std::uint32_t>(ny),
                    static_cast<std::uint32_t>(nz),
                    node.depth);
                changed = true;
            }
        }
    }
}
```

## Recommended Implementation Plan

### Phase 1: Fix GPU Halo Generation (Short-term)

Update [manifold_dual_contouring.cl](gladius/src/kernel/manifold_dual_contouring.cl) to implement iterative halo creation:

1. **Modify `count_halo_neighbors` kernel** to check for edge-sharing neighbors specifically:
   ```c
   // Instead of 26-neighborhood, check only the 4 cells per edge
   for (int edge = 0; edge < 12; edge++)
   {
       if (!(node.edgeMask & (1 << edge))) continue;
       
       // Get 4 cells for this edge
       int4 neighbors[4] = getEdgeNeighbors(coords, edge);
       
       for (int i = 0; i < 4; i++)
       {
           if (neighbors[i].x < 0 || neighbors[i].x > maxCoord ||
               neighbors[i].y < 0 || neighbors[i].y > maxCoord ||
               neighbors[i].z < 0 || neighbors[i].z > maxCoord)
               continue;
           
           ulong nMorton = encodeMorton3(neighbors[i].x, neighbors[i].y, neighbors[i].z);
           if (findNodeByMorton(nodes, numNodes, nMorton) < 0)
               count++;
       }
   }
   ```

2. **Add iterative loop** in [ManifoldDualContouringProgram.cpp](gladius/src/compute/ManifoldDualContouringProgram.cpp):
   ```cpp
   void ManifoldDualContouringProgram::addHaloNodes(...)
   {
       size_t iteration = 0;
       size_t previousNodeCount = 0;
       constexpr size_t MAX_ITERATIONS = 5;
       
       while (iteration < MAX_ITERATIONS)
       {
           // Sort for binary search
           sortOctreeByMorton(octreeBuffer, nodeCount);
           
           // Count + emit halos (existing logic)
           // ... count_halo_neighbors ...
           // ... emit_halo_neighbors ...
           // ... deduplicate + merge ...
           
           if (nodeCount == previousNodeCount)
           {
               std::cout << "Halo creation converged after " << iteration << " iterations" << std::endl;
               break;
           }
           
           previousNodeCount = nodeCount;
           ++iteration;
       }
   }
   ```

3. **Prevent halo nodes from emitting quads** by modifying [count_quads](gladius/src/kernel/manifold_dual_contouring.cl:1318):
   ```c
   __kernel void count_quads(...)
   {
       // Check if this is a halo node (marked in padding[0])
       if (node.padding[0] == 1)
       {
           quadCounts[id] = 0;
           return;
       }
       
       // ... rest of logic ...
   }
   ```

### Phase 2: Complete GlobalMortonOctree (Long-term)

Enable and fix the hierarchical approach:

1. **Extend balancing** to include edge-adjacent neighbors (code above)

2. **Add corner-adjacent neighbors** for complete 26-neighborhood:
   ```cpp
   // 8 corner-adjacent neighbors
   std::array<std::tuple<int, int, int>, 8> const cornerOffsets = {{
       {1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {1, -1, -1},
       {-1, 1, 1}, {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1}
   }};
   ```

3. **Re-enable in config** ([SurfaceExtractionOptions.h](gladius/src/io/SurfaceExtractionOptions.h)):
   ```cpp
   bool enableHierarchicalOctree{true};  // Change from false
   ```

4. **Test with admesh validation**

### Phase 3: Verification & Testing

1. **Unit tests for edge neighbor computation**:
   ```cpp
   TEST(EdgeNeighbors, AllEdgesHave4Cells)
   {
       for (int edge = 0; edge < 12; ++edge)
       {
           auto neighbors = getEdgeNeighbors({5,5,5}, edge, 8);
           EXPECT_EQ(neighbors.size(), 4);
           
           // Verify uniqueness
           std::set<std::tuple<int,int,int>> unique(neighbors.begin(), neighbors.end());
           EXPECT_EQ(unique.size(), 4);
       }
   }
   ```

2. **Integration test for watertightness**:
   ```cpp
   TEST(ManifoldDC, SphereInCage_Watertight)
   {
       auto mesh = generateMesh("testdata/SphereInACage.3mf");
       auto metrics = validateMesh(mesh);
       
       EXPECT_EQ(metrics.boundaryEdges, 0) << "Mesh should be watertight";
       EXPECT_EQ(metrics.parts, 2) << "Should have 2 parts (sphere + cage)";
   }
   ```

3. **Performance benchmarks**: Measure overhead of iterative halo creation

## Alternative: Simplified Edge-Only Halo

If full 26-neighborhood is too expensive, create halos **only for edges that need them**:

```cpp
void addEdgeHalos(std::vector<OctreeNode>& nodes, uint32_t maxCoord, uint8_t depth)
{
    std::unordered_set<uint64_t> existing;
    for (auto const& node : nodes)
        existing.insert(node.mortonCode);
    
    std::vector<OctreeNode> halos;
    
    for (auto const& node : nodes)
    {
        if (node.edgeMask == 0) continue;
        
        uint3 coords = decodeMorton3(node.mortonCode);
        
        // Only process owned edges (0, 3, 8 for MIN corner ownership)
        for (int edge : {0, 3, 8})
        {
            if (!(node.edgeMask & (1 << edge))) continue;
            
            auto neighbors = getEdgeNeighbors(coords, edge, depth);
            
            for (auto const& [nx, ny, nz] : neighbors)
            {
                if (nx < 0 || nx > maxCoord || ny < 0 || ny > maxCoord || 
                    nz < 0 || nz > maxCoord)
                    continue;
                
                uint64_t morton = encodeMorton3(nx, ny, nz);
                
                if (existing.find(morton) == existing.end())
                {
                    OctreeNode halo;
                    halo.mortonCode = morton;
                    halo.depth = depth;
                    halo.edgeMask = 0;  // Halos don't emit
                    halo.internalMask = 0;
                    
                    halos.push_back(halo);
                    existing.insert(morton);
                }
            }
        }
    }
    
    nodes.insert(nodes.end(), halos.begin(), halos.end());
}
```

This avoids the cascading problem by:
1. Only creating halos for cells that directly need them (edges 0, 3, 8)
2. Not assigning edgeMask to halos (they don't try to emit quads)
3. Single-pass execution (no iteration needed)

## References

### Literature

1. **Dual Contouring of Hermite Data** (Ju et al., SIGGRAPH 2002)
   - Original DC algorithm, defines QEF vertex placement
   - Assumes uniform grid (all cells at same depth)

2. **Manifold Dual Contouring** (Schaefer et al., IEEE TVCG 2007)
   - Multiple vertices per cell for CSG discontinuities
   - Gradient angle threshold for component detection
   - Does NOT address watertightness in adaptive octrees

3. **Dual Marching Cubes** (Schaefer & Warren, IEEE Visualization 2004)
   - Guarantees watertightness by ensuring all edges have 4 cells
   - Uses "crack-free" subdivision strategy
   - Relevant for adaptive octree construction

4. **Isosurface Stuffing** (Labelle & Shewchuk, ACM TOG 2007)
   - Guarantees topologically correct meshes
   - Uses complete boundary layer beyond surface
   - Octree subdivision ensures all neighbors exist

5. **Feature Preserving Octree-Based Hexahedral Meshing** (Ito et al., 2009)
   - 2:1 balanced octrees with neighbor completeness
   - Edge-adjacent and corner-adjacent neighbor requirements

### Implementation References

- [ManifoldDualContouringProgram.cpp:259](gladius/src/compute/ManifoldDualContouringProgram.cpp#L259) - Current halo implementation
- [manifold_dual_contouring.cl:1118](gladius/src/kernel/manifold_dual_contouring.cl#L1118) - Halo kernels
- [GlobalMortonOctree.cpp:500](gladius/src/compute/GlobalMortonOctree.cpp#L500) - Balancing algorithm
- [ManifoldDualContouring_tests.cpp:2828](gladius/tests/unittests/ManifoldDualContouring_tests.cpp#L2828) - Cascading problem test

## Conclusion

The path to watertight meshes is clear:

1. **Immediate fix**: Implement iterative halo generation in GPU path (Phase 1)
2. **Long-term solution**: Complete GlobalMortonOctree with edge/corner neighbors (Phase 2)
3. **Alternative**: Edge-only halos if performance is critical

All required infrastructure exists; the fix is primarily adding the iterative loop and edge-neighbor completeness checks.
