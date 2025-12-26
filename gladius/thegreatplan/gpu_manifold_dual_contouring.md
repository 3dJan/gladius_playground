# GPU-Based Manifold Dual Contouring Plan

## 1. Overview

This document outlines the strategy for implementing **Manifold Dual Contouring** using OpenCL. The goal is to generate **watertight, printable meshes** from implicit surfaces (SDFs) directly on the GPU.

### Current Status: Chunked Approach (Limitation)
The current chunked implementation produces 21 separate parts instead of 2 expected for a sphere-in-cage model. This is because:
- Each chunk builds its own octree with different cell boundaries
- Triangles at chunk boundaries don't share edges
- Simple vertex welding cannot fix topological disconnects
- Gap-filling bridge triangles help but don't achieve full watertightness

### Target: Manifold Dual Contouring
A single global octree that:
- Maintains consistent cell boundaries across the entire domain
- Generates topology-correct mesh with shared vertices at all boundaries
- Produces watertight, manifold meshes suitable for 3D printing

---

## 2. Problem Analysis: Why Chunking Fails

### Root Cause
```
Chunk A octree:          Chunk B octree:
+---+---+---+            +---+---+---+
|   | X |   |            |   | Y |   |  ← Different cell boundaries!
+---+---+---+            +---+---+---+
    ↑                        ↑
    Cell boundary at x=50.0  Cell boundary at x=50.1
```

When chunks have different octree subdivisions:
1. Vertices are placed at different positions
2. Edges don't align across chunk boundaries
3. Triangles can't share vertices → gaps/T-junctions

### Solution: Global Hierarchical Octree
- Single octree covering entire bounding box
- All chunks use the **same** cell boundaries
- Process chunks sequentially but generate consistent topology

---

## 3. Architecture: Manifold Dual Contouring

### 3.1 Global Morton-Indexed Octree

All octree cells use **global Morton codes** derived from the full bounding box:

```cpp
// Morton code encodes global position, not chunk-local
uint64_t computeGlobalMorton(Eigen::Vector3f pos, 
                              Eigen::Vector3f globalBboxMin,
                              Eigen::Vector3f globalBboxSize,
                              uint32_t maxDepth)
{
    // Normalize to [0, 2^maxDepth) range across ENTIRE bbox
    uint32_t cellsPerAxis = 1U << maxDepth;
    uint32_t ix = ((pos.x() - globalBboxMin.x()) / globalBboxSize.x()) * cellsPerAxis;
    uint32_t iy = ((pos.y() - globalBboxMin.y()) / globalBboxSize.y()) * cellsPerAxis;
    uint32_t iz = ((pos.z() - globalBboxMin.z()) / globalBboxSize.z()) * cellsPerAxis;
    return encodeMorton3D(ix, iy, iz);
}
```

### 3.2 Data Structures

```cpp
// Global vertex registry - ensures shared vertices at cell boundaries
struct GlobalVertexRegistry
{
    // Morton code of cell → vertex index in global mesh
    std::unordered_map<uint64_t, uint32_t> cellToVertex;
    
    // Morton code of edge → vertex index (for edge vertices in manifold DC)
    std::unordered_map<uint64_t, uint32_t> edgeToVertex;
};

// Octree node with global indexing
struct HierarchicalOctreeNode
{
    uint64_t mortonCode;           // Global Morton code
    uint8_t depth;                 // 0 = root, maxDepth = leaf
    float cornerValues[8];         // SDF at 8 corners
    uint8_t edgeMask;              // Which edges have zero-crossings
    uint8_t componentCount;        // Number of surface components (for manifold)
    Eigen::Vector3f vertexPositions[4];  // Up to 4 vertices per cell (manifold DC)
    uint32_t vertexIndices[4];     // Global vertex indices
};
```

### 3.3 Pipeline Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 1: Global Octree Construction (Level-by-Level GPU)          │
│  ┌─────────────┐    ┌───────────────────┐    ┌──────────────────┐  │
│  │ Evaluate    │ →  │ Detect           │ →  │ Create Children  │  │
│  │ Corners GPU │    │ Intersections GPU│    │ (CPU)            │  │
│  └─────────────┘    └───────────────────┘    └──────────────────┘  │
│         ↓                                                           │
│  Repeat for each depth level until maxDepth                        │
└─────────────────────────────────────────────────────────────────────┘
                               ↓
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 2: Adaptive Refinement (Iterative GPU)                      │
│  ┌─────────────┐    ┌───────────────────┐    ┌──────────────────┐  │
│  │ Estimate    │ →  │ Mark High        │ →  │ Subdivide Marked │  │
│  │ Curvature   │    │ Curvature Leaves │    │ (Repeat Phase 1) │  │
│  └─────────────┘    └───────────────────┘    └──────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                               ↓
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 3: Manifold Analysis & Vertex Generation                    │
│  ┌─────────────┐    ┌───────────────────┐    ┌──────────────────┐  │
│  │ Analyze     │ →  │ Refine Zero      │ →  │ Solve QEF        │  │
│  │ Components  │    │ Crossings (GPU)  │    │ (GPU/CPU)        │  │
│  └─────────────┘    └───────────────────┘    └──────────────────┘  │
│         ↓                                                           │
│  Register vertices in GlobalVertexRegistry (Morton-indexed)        │
└─────────────────────────────────────────────────────────────────────┘
                               ↓
┌─────────────────────────────────────────────────────────────────────┐
│  Phase 4: Watertight Mesh Generation                               │
│  ┌─────────────┐    ┌───────────────────┐    ┌──────────────────┐  │
│  │ Find Shared │ →  │ Generate Quads   │ →  │ Validate Mesh    │  │
│  │ Edges       │    │ (Correct Winding)│    │ (Manifold Check) │  │
│  └─────────────┘    └───────────────────┘    └──────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. Phase 1: Global Octree Construction

### 4.1 Level-by-Level Breadth-First Traversal

```cpp
void HierarchicalOctreeBuilder::buildOctree()
{
    // Initialize with global bounding box
    m_globalBboxMin = computeGlobalBboxMin();
    m_globalBboxMax = computeGlobalBboxMax();
    m_globalBboxSize = m_globalBboxMax - m_globalBboxMin;
    
    // Calculate required depth for minFeatureSize
    m_requiredDepth = calculateRequiredDepth(m_globalBboxSize.maxCoeff(), 
                                              m_config.minFeatureSize);
    
    // Root node covers entire bbox
    m_levels[0].nodes.push_back(createRootNode());
    
    // Process level by level
    for (size_t depth = 0; depth < m_requiredDepth; ++depth)
    {
        auto& currentLevel = m_levels[depth];
        
        if (currentLevel.nodes.empty())
            break;
        
        // GPU: Batch evaluate all corners at this level
        evaluateCornersGPU(currentLevel);
        
        // GPU: Detect which nodes intersect surface
        detectIntersectionsGPU(currentLevel);
        
        // CPU: Create children for intersecting nodes
        for (auto& node : currentLevel.nodes)
        {
            if (node.isIntersecting)
            {
                createChildren(node, m_levels[depth + 1]);
            }
        }
    }
}
```

### 4.2 GPU Corner Evaluation Kernel

```c
__kernel void evaluateOctreeLevel(
    __global const float* nodeBoundsMin,  // 3 floats per node
    __global const float* nodeBoundsMax,  // 3 floats per node
    __global float* cornerValues,          // 8 floats per node (output)
    const uint nodeCount,
    const float isoValue
    PAYLOAD_ARGS)
{
    uint nodeId = get_global_id(0);
    if (nodeId >= nodeCount) return;
    
    float3 bmin = vload3(nodeId, nodeBoundsMin);
    float3 bmax = vload3(nodeId, nodeBoundsMax);
    
    // Evaluate 8 corners
    for (int c = 0; c < 8; c++)
    {
        float3 corner = (float3)(
            (c & 1) ? bmax.x : bmin.x,
            (c & 2) ? bmax.y : bmin.y,
            (c & 4) ? bmax.z : bmin.z
        );
        
        float sdf = model(corner, PASS_PAYLOAD_ARGS).w - isoValue;
        cornerValues[nodeId * 8 + c] = sdf;
    }
}
```

### 4.3 Intersection Detection Kernel

```c
// Edge corner indices for octree cell
__constant int edgeCorners[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},  // X-aligned
    {0, 2}, {1, 3}, {4, 6}, {5, 7},  // Y-aligned
    {0, 4}, {1, 5}, {2, 6}, {3, 7}   // Z-aligned
};

__kernel void detectIntersections(
    __global const float* cornerValues,
    __global uchar* subdivisionFlags,
    __global uint* edgeMasks,
    const uint nodeCount)
{
    uint nodeId = get_global_id(0);
    if (nodeId >= nodeCount) return;
    
    uint edgeMask = 0;
    bool hasIntersection = false;
    
    for (int e = 0; e < 12; e++)
    {
        float v0 = cornerValues[nodeId * 8 + edgeCorners[e][0]];
        float v1 = cornerValues[nodeId * 8 + edgeCorners[e][1]];
        
        if (v0 * v1 < 0.0f)  // Sign change
        {
            hasIntersection = true;
            edgeMask |= (1u << e);
        }
    }
    
    subdivisionFlags[nodeId] = hasIntersection ? 1 : 0;
    edgeMasks[nodeId] = edgeMask;
}
```

---

## 5. Phase 2: Adaptive Refinement

### 5.1 Curvature Estimation Kernel

```c
__kernel void estimateCurvature(
    __global const float* leafCenters,
    __global float* curvatureMetrics,
    const uint leafCount,
    const float epsilon
    PAYLOAD_ARGS)
{
    uint leafId = get_global_id(0);
    if (leafId >= leafCount) return;
    
    float3 center = vload3(leafId, leafCenters);
    
    // Central gradient at center
    float3 gradCenter = computeGradient(center, epsilon, PASS_PAYLOAD_ARGS);
    float3 normCenter = normalize(gradCenter);
    
    // Gradients at 6 axis-aligned neighbors
    float3 offsets[6] = {
        (float3)(epsilon, 0, 0), (float3)(-epsilon, 0, 0),
        (float3)(0, epsilon, 0), (float3)(0, -epsilon, 0),
        (float3)(0, 0, epsilon), (float3)(0, 0, -epsilon)
    };
    
    float variance = 0.0f;
    for (int i = 0; i < 6; i++)
    {
        float3 gradNeighbor = computeGradient(center + offsets[i], epsilon, PASS_PAYLOAD_ARGS);
        float3 normNeighbor = normalize(gradNeighbor);
        float3 diff = normCenter - normNeighbor;
        variance += dot(diff, diff);
    }
    
    curvatureMetrics[leafId] = variance / 6.0f;
}
```

### 5.2 Refinement Loop

```cpp
void HierarchicalOctreeBuilder::performAdaptiveRefinement()
{
    for (size_t pass = 0; pass < m_config.refinementPasses; ++pass)
    {
        // Collect current leaf nodes
        auto leaves = collectIntersectingLeaves();
        
        // GPU: Estimate curvature
        std::vector<float> curvatures;
        estimateCurvatureGPU(leaves, curvatures);
        
        // Mark high-curvature leaves for subdivision
        std::vector<OctreeNode*> toRefine;
        for (size_t i = 0; i < leaves.size(); ++i)
        {
            if (curvatures[i] > m_config.curvatureThreshold &&
                leaves[i]->depth < m_config.maxDepth)
            {
                toRefine.push_back(leaves[i]);
            }
        }
        
        if (toRefine.empty())
            break;
        
        // Subdivide marked nodes (using Phase 1 pipeline)
        for (auto* node : toRefine)
        {
            createChildren(*node, m_levels[node->depth + 1]);
        }
        
        // Evaluate new children
        evaluateCornersGPU(m_levels[...]);
        detectIntersectionsGPU(m_levels[...]);
    }
}
```

---

## 6. Phase 3: Manifold Analysis & Vertex Generation

### 6.1 Manifold Cell Analysis

For watertight meshes, cells with complex topology need multiple vertices:

```cpp
// Lookup table: 256 sign configurations → component count
static const uint8_t COMPONENT_COUNT_LUT[256] = { /* precomputed */ };

// Analyzes cell topology to determine vertex count
uint8_t analyzeManifoldComponents(uint8_t signMask, uint32_t edgeMask)
{
    // Simple case: single component
    uint8_t baseCount = COMPONENT_COUNT_LUT[signMask];
    
    // Additional analysis for edge connectivity
    // (Uses DSU or graph analysis on 12 edges)
    return baseCount;
}
```

### 6.2 Zero-Crossing Refinement (GPU Bisection)

```c
__kernel void refineZeroCrossings(
    __global const float3* edgeStarts,
    __global const float3* edgeEnds,
    __global float3* refinedPositions,
    const uint edgeCount,
    const float tolerance,
    const int maxIterations
    PAYLOAD_ARGS)
{
    uint edgeId = get_global_id(0);
    if (edgeId >= edgeCount) return;
    
    float3 lo = edgeStarts[edgeId];
    float3 hi = edgeEnds[edgeId];
    
    float vLo = model(lo, PASS_PAYLOAD_ARGS).w;
    float vHi = model(hi, PASS_PAYLOAD_ARGS).w;
    
    // Bisection
    for (int i = 0; i < maxIterations; i++)
    {
        float3 mid = (lo + hi) * 0.5f;
        float vMid = model(mid, PASS_PAYLOAD_ARGS).w;
        
        if (fabs(vMid) < tolerance)
        {
            refinedPositions[edgeId] = mid;
            return;
        }
        
        if (vMid * vLo < 0.0f)
        {
            hi = mid;
            vHi = vMid;
        }
        else
        {
            lo = mid;
            vLo = vMid;
        }
    }
    
    refinedPositions[edgeId] = (lo + hi) * 0.5f;
}
```

### 6.3 QEF Vertex Placement

```cpp
Eigen::Vector3f solveQEF(std::vector<HermiteSample> const& samples,
                         BoundingBox const& cellBounds)
{
    if (samples.size() < 3)
    {
        // Underconstrained: use mass point
        Eigen::Vector3f massPoint = Eigen::Vector3f::Zero();
        for (auto const& s : samples)
            massPoint += s.position;
        return cellBounds.clamp(massPoint / samples.size());
    }
    
    // Build ATA and ATb for least squares
    Eigen::Matrix3f ATA = Eigen::Matrix3f::Zero();
    Eigen::Vector3f ATb = Eigen::Vector3f::Zero();
    
    for (auto const& sample : samples)
    {
        Eigen::Vector3f n = sample.gradient.normalized();
        ATA += n * n.transpose();
        ATb += n * n.dot(sample.position);
    }
    
    // Solve with SVD for stability
    Eigen::JacobiSVD<Eigen::Matrix3f> svd(ATA, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Vector3f vertex = svd.solve(ATb);
    
    // Clamp to cell bounds
    return cellBounds.clamp(vertex);
}
```

### 6.4 Global Vertex Registration

```cpp
void registerVertexGlobally(uint64_t cellMorton, 
                            Eigen::Vector3f const& position,
                            GlobalVertexRegistry& registry,
                            std::vector<Eigen::Vector3f>& vertices)
{
    auto it = registry.cellToVertex.find(cellMorton);
    if (it == registry.cellToVertex.end())
    {
        // New vertex
        uint32_t index = static_cast<uint32_t>(vertices.size());
        vertices.push_back(position);
        registry.cellToVertex[cellMorton] = index;
    }
    // Else: vertex already registered (shared by adjacent chunk processing)
}
```

---

## 7. Phase 4: Watertight Mesh Generation

### 7.1 Finding Shared Edges (Critical for Watertightness)

```cpp
// For each internal edge in the octree, find the 4 cells that share it
std::vector<QuadFace> findSharedEdges(std::vector<OctreeNode> const& leaves,
                                       GlobalVertexRegistry const& registry)
{
    std::vector<QuadFace> quads;
    
    // Edge → list of cells sharing this edge
    std::unordered_map<uint64_t, std::vector<uint64_t>> edgeToCells;
    
    for (auto const& leaf : leaves)
    {
        if (!leaf.isIntersecting)
            continue;
        
        // Register this cell for each of its 12 edges
        for (int e = 0; e < 12; e++)
        {
            if (!(leaf.edgeMask & (1 << e)))
                continue;  // No crossing on this edge
            
            uint64_t edgeMorton = computeEdgeMorton(leaf.mortonCode, e, leaf.depth);
            edgeToCells[edgeMorton].push_back(leaf.mortonCode);
        }
    }
    
    // Create quads where exactly 4 cells share an edge
    for (auto const& [edgeMorton, cells] : edgeToCells)
    {
        if (cells.size() == 4)
        {
            QuadFace quad;
            for (int i = 0; i < 4; i++)
            {
                quad.vertexIndices[i] = registry.cellToVertex.at(cells[i]);
            }
            quads.push_back(quad);
        }
        else if (cells.size() != 2 && cells.size() != 0)
        {
            // Warning: T-junction or non-manifold edge
            logWarning("Edge has {} adjacent cells (expected 4)", cells.size());
        }
    }
    
    return quads;
}
```

### 7.2 Winding Order Correction

```cpp
void correctWindingOrder(QuadFace& quad,
                          std::vector<Eigen::Vector3f> const& vertices,
                          std::vector<Eigen::Vector3f> const& normals)
{
    // Compute quad center
    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    for (int i = 0; i < 4; i++)
        center += vertices[quad.vertexIndices[i]];
    center /= 4.0f;
    
    // Get average normal from vertices
    Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();
    for (int i = 0; i < 4; i++)
        avgNormal += normals[quad.vertexIndices[i]];
    avgNormal.normalize();
    
    // Compute face normal from cross product
    Eigen::Vector3f v0 = vertices[quad.vertexIndices[0]];
    Eigen::Vector3f v1 = vertices[quad.vertexIndices[1]];
    Eigen::Vector3f v2 = vertices[quad.vertexIndices[2]];
    Eigen::Vector3f faceNormal = (v1 - v0).cross(v2 - v0);
    
    // Flip if facing wrong way
    if (faceNormal.dot(avgNormal) < 0.0f)
    {
        std::swap(quad.vertexIndices[1], quad.vertexIndices[3]);
    }
}
```

### 7.3 Final Mesh Validation

```cpp
struct MeshValidation
{
    bool isManifold;
    bool isWatertight;
    size_t boundaryEdges;
    size_t nonManifoldEdges;
    size_t degenerateTriangles;
    int eulerCharacteristic;
};

MeshValidation validateMesh(std::vector<Eigen::Vector3f> const& vertices,
                             std::vector<uint32_t> const& indices)
{
    MeshValidation result{};
    
    // Build edge → face count map
    std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
    
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        uint32_t v0 = indices[i], v1 = indices[i+1], v2 = indices[i+2];
        
        // Check for degenerate triangles
        if (v0 == v1 || v1 == v2 || v2 == v0)
        {
            result.degenerateTriangles++;
            continue;
        }
        
        auto addEdge = [&](uint32_t a, uint32_t b) {
            auto edge = std::minmax(a, b);
            edgeCount[edge]++;
        };
        
        addEdge(v0, v1);
        addEdge(v1, v2);
        addEdge(v2, v0);
    }
    
    // Analyze edges
    for (auto const& [edge, count] : edgeCount)
    {
        if (count == 1)
            result.boundaryEdges++;
        else if (count > 2)
            result.nonManifoldEdges++;
    }
    
    result.isManifold = (result.nonManifoldEdges == 0);
    result.isWatertight = (result.boundaryEdges == 0);
    
    // Euler characteristic: V - E + F
    size_t V = vertices.size();
    size_t E = edgeCount.size();
    size_t F = indices.size() / 3;
    result.eulerCharacteristic = static_cast<int>(V) - static_cast<int>(E) + static_cast<int>(F);
    
    return result;
}
```

---

## 8. Memory-Efficient Chunk Processing

For very large models, process the global octree in spatial chunks while maintaining watertightness:

### 8.1 Chunk Strategy

```cpp
struct ChunkProcessor
{
    GlobalVertexRegistry m_globalRegistry;  // Shared across all chunks
    std::vector<Eigen::Vector3f> m_globalVertices;
    std::vector<uint32_t> m_globalIndices;
    
    void processChunk(BoundingBox const& chunkBounds, size_t depth)
    {
        // Collect octree nodes within this chunk
        auto nodesInChunk = collectNodesInBounds(chunkBounds);
        
        // Generate vertices for these nodes
        for (auto& node : nodesInChunk)
        {
            if (!node.hasVertex())
            {
                // Solve QEF and register vertex globally
                auto vertex = solveQEF(node);
                registerVertexGlobally(node.mortonCode, vertex, 
                                       m_globalRegistry, m_globalVertices);
            }
        }
        
        // Generate quads for edges ENTIRELY within this chunk
        // (Edges on chunk boundaries are deferred to final pass)
        for (auto& node : nodesInChunk)
        {
            for (int e = 0; e < 3; e++)  // Only process "owned" edges
            {
                if (isEdgeEntirelyInChunk(node, e, chunkBounds))
                {
                    generateQuadForEdge(node, e);
                }
            }
        }
    }
    
    void finalBoundaryPass()
    {
        // Process all edges that span chunk boundaries
        // These edges were skipped during chunk processing
        for (auto& edge : collectBoundaryEdges())
        {
            generateQuadForEdge(edge);
        }
    }
};
```

### 8.2 Morton-Based Chunk Ordering

Process chunks in Morton order to maximize cache coherency:

```cpp
std::vector<BoundingBox> generateChunksInMortonOrder(BoundingBox const& globalBbox,
                                                      size_t chunksPerAxis)
{
    std::vector<std::pair<uint64_t, BoundingBox>> chunksWithMorton;
    
    for (size_t iz = 0; iz < chunksPerAxis; iz++)
    for (size_t iy = 0; iy < chunksPerAxis; iy++)
    for (size_t ix = 0; ix < chunksPerAxis; ix++)
    {
        uint64_t morton = encodeMorton3D(ix, iy, iz);
        BoundingBox chunkBounds = computeChunkBounds(globalBbox, ix, iy, iz, chunksPerAxis);
        chunksWithMorton.emplace_back(morton, chunkBounds);
    }
    
    // Sort by Morton code for spatial locality
    std::sort(chunksWithMorton.begin(), chunksWithMorton.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    
    std::vector<BoundingBox> result;
    for (auto& [morton, bounds] : chunksWithMorton)
        result.push_back(bounds);
    
    return result;
}
```

---

## 9. Quality Presets

```cpp
struct HierarchicalMDCConfig
{
    // Octree depth
    size_t initialDepth{5};
    size_t maxDepth{9};
    
    // Adaptive refinement
    size_t refinementPasses{2};
    float curvatureThreshold{0.3f};
    
    // Zero-crossing refinement
    size_t bisectionIterations{10};
    float zeroCrossingTolerance{1e-5f};
    
    // Feature preservation
    float minFeatureSize{0.1f};  // mm
    
    // Memory management
    size_t maxNodesPerChunk{100000};
    bool enableChunking{true};
};

HierarchicalMDCConfig getPreset(Quality quality)
{
    HierarchicalMDCConfig config;
    
    switch (quality)
    {
    case Quality::Draft:
        config.initialDepth = 4;
        config.maxDepth = 6;
        config.refinementPasses = 0;
        config.bisectionIterations = 5;
        break;
        
    case Quality::Balanced:
        config.initialDepth = 5;
        config.maxDepth = 8;
        config.refinementPasses = 1;
        config.curvatureThreshold = 0.4f;
        break;
        
    case Quality::Fine:
        config.initialDepth = 6;
        config.maxDepth = 9;
        config.refinementPasses = 2;
        config.curvatureThreshold = 0.25f;
        break;
        
    case Quality::UltraFine:
        config.initialDepth = 7;
        config.maxDepth = 10;
        config.refinementPasses = 3;
        config.curvatureThreshold = 0.15f;
        config.bisectionIterations = 15;
        config.zeroCrossingTolerance = 1e-6f;
        break;
    }
    
    return config;
}
```

---

## 10. Implementation Roadmap

### Phase 1: Global Octree Foundation (Week 1)
- [ ] `HierarchicalOctreeNode` with global Morton codes
- [ ] `GlobalVertexRegistry` for shared vertex tracking
- [ ] Level-by-level GPU octree construction
- [ ] Corner evaluation kernel integration
- [ ] Intersection detection kernel

### Phase 2: Adaptive Refinement (Week 2)
- [ ] Curvature estimation kernel
- [ ] Iterative refinement loop
- [ ] Depth-limited subdivision
- [ ] Statistics and progress reporting

### Phase 3: Manifold Vertex Generation (Week 3)
- [ ] Manifold component analysis (LUT-based)
- [ ] GPU zero-crossing bisection
- [ ] QEF solver integration
- [ ] Global vertex registration

### Phase 4: Watertight Mesh Extraction (Week 4)
- [ ] Shared edge detection (Morton-based)
- [ ] Quad generation with correct winding
- [ ] Mesh validation (manifold/watertight checks)
- [ ] Boundary edge handling for chunks

### Phase 5: Testing & Validation (Week 5)
- [ ] Unit tests for each component
- [ ] Integration test: SphereInACage → 2 parts (sphere + cage)
- [ ] Integration test: ImplicitGyroid → 1 part (watertight)
- [ ] Admesh validation: 0 boundary edges, 0 non-manifold edges
- [ ] PrusaSlicer import test: no errors highlighted
a
### Phase 6: Optimization (Week 6)
- [ ] GPU kernel profiling
- [ ] Memory coalescing optimization
- [ ] Corner value caching/deduplication
- [ ] Parallel chunk processing

---

## 11. Success Criteria

A successful implementation produces meshes that:

1. **Pass admesh validation:**
   - `Parts: 2` for SphereInACage (sphere + cage, not 21)
   - `Disconnected facets: 0`
   - `Degenerate facets: 0`
   - `Boundary edges: 0` (watertight)

2. **Pass PrusaSlicer import:**
   - No red error highlights on mesh surfaces
   - Correct slice previews

3. **Meet performance targets:**
   - Gyroid 50mm cube, Balanced preset: < 200ms
   - SphereInACage, Fine preset: < 500ms

4. **Handle large models:**
   - 200mm models with 0.1mm minFeatureSize
   - Memory usage < 2GB

---

## 12. Watertightness Research & Open Edge Fixes

### Current Issue: Meshes with Open Edges

The current implementation produces meshes with boundary edges (non-watertight) because of **incomplete halo node generation**. When a surface-intersecting cell needs neighbors to form quads, but those neighbors don't exist (entirely outside surface), edges become boundaries.

**Root Cause: Cascading Halo Problem**
- Surface cells create halo nodes for missing neighbors ✅
- Halo nodes inherit edgeMask bits ✅
- Halo nodes try to emit quads but lack their own neighbors ❌
- Result: Skipped quads → holes

### Research Summary

See detailed analysis in [watertight_dual_contouring_research.md](../../../thegreatplan/watertight_dual_contouring_research.md)

**Key Findings:**
1. **Root cause identified**: Missing second-order neighbors (halos need halos)
2. **Solution exists**: Iterative halo generation until convergence
3. **Alternative approach**: Edge-centric neighbor completeness
4. **Working baseline**: Chunked approach produces ~1% boundary edges (acceptable for printing)

**Implementation Status:**
- ✅ 2:1 octree balancing (prevents T-junctions)
- ✅ Global Morton indexing (disabled due to halo issue)
- ✅ Manifold DC component analysis
- ✅ Edge ownership rules
- ⚠️ Halo generation (partial - needs iteration)

### Recommended Fix

**Phase 1: GPU Halo Iteration** (Short-term)
```cpp
// In ManifoldDualContouringProgram::addHaloNodes()
size_t iteration = 0;
size_t previousNodeCount = 0;
constexpr size_t MAX_ITERATIONS = 5;

while (iteration < MAX_ITERATIONS)
{
    sortOctreeByMorton(octreeBuffer, nodeCount);
    
    // Count + emit halos for edges that need 4 cells
    // (existing logic, but check edge-neighbors not 26-neighborhood)
    
    if (nodeCount == previousNodeCount)
    {
        std::cout << "Halo creation converged after " << iteration << " iterations" << std::endl;
        break;
    }
    
    previousNodeCount = nodeCount;
    ++iteration;
}
```

**Phase 2: Complete GlobalMortonOctree** (Long-term)
- Extend balancing to edge-adjacent neighbors (12 additional per cell)
- Add corner-adjacent neighbors (8 additional) for complete 26-neighborhood
- Re-enable `enableHierarchicalOctree = true`

**Phase 3: Validation**
- Unit tests for edge neighbor computation (12 edges × 4 cells)
- Integration tests with admesh: `EXPECT_EQ(boundaryEdges, 0)`
- Performance benchmarks for iteration overhead

### References to Implementation

**Current Code:**
- Halo generation: [ManifoldDualContouringProgram.cpp:259](../src/compute/ManifoldDualContouringProgram.cpp#L259)
- Halo kernels: [manifold_dual_contouring.cl:1118](../src/kernel/manifold_dual_contouring.cl#L1118)
- GlobalMortonOctree balancing: [GlobalMortonOctree.cpp:500](../src/compute/GlobalMortonOctree.cpp#L500)
- Test documenting issue: [ManifoldDualContouring_tests.cpp:2828](../tests/unittests/ManifoldDualContouring_tests.cpp#L2828)

**Why GlobalMortonOctree is disabled:**
```cpp
// GlobalMortonOctree.cpp:21-26
// Currently disabled because edge-to-cells mapping fails when neighbor cells don't
// intersect the surface (common at boundaries), causing non-manifold meshes.
```

This is fixable by completing the neighbor creation in balancing phase.

---

## 13. Original References

1. Ju, T., et al. "Dual Contouring of Hermite Data" (SIGGRAPH 2002)
2. Schaefer, S., et al. "Manifold Dual Contouring" (IEEE TVCG 2007)
3. Schaefer, S., Warren, J. "Dual Marching Cubes" (IEEE Visualization 2004) - Watertight guarantees
4. Labelle, F., Shewchuk, J. "Isosurface Stuffing" (ACM TOG 2007) - Complete boundary layers
5. Lewiner, T. "Efficient Implementation of Marching Cubes" (2003)
6. Gladius existing code: `DualContouringQef.cpp`, `ManifoldDualContouringGpu.cpp`
7. Gladius kernels: `dual_contouring_sampling.cl`, `hierarchical_dc.cl`

