# Hierarchical Dual Contouring with Dynamic SDF Queries

## Executive Summary

Replace the fixed voxel grid approach with a **fully hierarchical, query-driven octree** that adaptively refines based on actual surface features by querying the SDF at runtime. This eliminates pre-sampling artifacts and enables arbitrary detail levels.

## Core Concept

Instead of:
1. ❌ Pre-compute fixed 129³ SDF grid
2. ❌ Build octree from pre-sampled values
3. ❌ Limited by initial grid resolution

Do:
1. ✅ Start with single root cube encompassing model bounds
2. ✅ Query SDF corners on-demand during octree traversal
3. ✅ Subdivide based on actual surface complexity detected
4. ✅ Refine iteratively with multiple query passes

## Hierarchical Architecture

### Phase 1: Coarse Octree Construction (GPU-Parallel)

```
Root Node (entire bounds)
├─ Query 8 corners on GPU (batched)
├─ Detect sign changes (zero-crossing detection)
└─ If intersecting → subdivide into 8 children
   ├─ Child 0: Query 8 corners (4 shared with parent/siblings)
   ├─ Child 1: Query 8 corners ...
   └─ Recurse until max depth or no intersection
```

**GPU Kernel Workflow:**
```cpp
// Kernel 1: Parallel corner evaluation for entire octree level
__kernel void evaluateOctreeLevel(
    __global const BoundingBox* nodeBounds,     // All nodes at current depth
    __global float* cornerValues,                // Output: 8 values per node
    const uint nodeCount,
    // SDF evaluation parameters...
)
{
    uint nodeId = get_global_id(0);
    if (nodeId >= nodeCount) return;
    
    BoundingBox bounds = nodeBounds[nodeId];
    
    // Evaluate all 8 corners for this node
    for (int i = 0; i < 8; i++)
    {
        float3 corner = computeCornerPosition(bounds, i);
        float distance = evaluateSdf(corner, ...);
        cornerValues[nodeId * 8 + i] = distance;
    }
}

// Kernel 2: Sign change detection and subdivision decision
__kernel void detectIntersections(
    __global const float* cornerValues,
    __global uchar* subdivisionFlags,           // Output: 1 if should subdivide
    __global float* featureMetrics,             // Curvature/gradient variance
    const uint nodeCount
)
{
    uint nodeId = get_global_id(0);
    if (nodeId >= nodeCount) return;
    
    // Check for sign changes across edges
    bool hasZeroCrossing = false;
    for (int edgeIdx = 0; edgeIdx < 12; edgeIdx++)
    {
        int c0 = edgeCorners[edgeIdx][0];
        int c1 = edgeCorners[edgeIdx][1];
        float v0 = cornerValues[nodeId * 8 + c0];
        float v1 = cornerValues[nodeId * 8 + c1];
        
        if (v0 * v1 < 0.0f)  // Sign change = surface crossing
        {
            hasZeroCrossing = true;
            break;
        }
    }
    
    subdivisionFlags[nodeId] = hasZeroCrossing ? 1 : 0;
}
```

**CPU Orchestration:**
```cpp
class HierarchicalDualContouring
{
    std::vector<OctreeLevel> m_levels;  // Level 0 = root, Level N = leaves
    
    void buildOctree()
    {
        // Level 0: Single root node
        m_levels[0].nodes.push_back(createRootNode(m_bounds));
        
        for (size_t depth = 0; depth < m_maxDepth; ++depth)
        {
            auto& currentLevel = m_levels[depth];
            
            // GPU: Evaluate all corners for this level in parallel
            evaluateOctreeLevelGPU(currentLevel);
            
            // GPU: Detect which nodes need subdivision
            detectIntersectionsGPU(currentLevel);
            
            // CPU: Create child nodes for next level
            auto& nextLevel = m_levels[depth + 1];
            for (auto& node : currentLevel.nodes)
            {
                if (node.shouldSubdivide)
                {
                    createChildren(node, nextLevel);
                }
            }
            
            if (nextLevel.nodes.empty())
                break;  // No more refinement needed
        }
    }
};
```

### Phase 2: Feature-Adaptive Refinement (Iterative)

After initial octree construction, perform multiple refinement passes:

```cpp
// Kernel 3: Curvature estimation for adaptive refinement
__kernel void estimateCurvature(
    __global const float4* leafPositions,       // Leaf vertex positions
    __global const float4* nodeCorners,         // 8 corners per leaf
    __global float* curvatureMetrics,           // Output: curvature measure
    const uint leafCount,
    const float gradientEpsilon
)
{
    uint leafId = get_global_id(0);
    if (leafId >= leafCount) return;
    
    float3 center = leafPositions[leafId].xyz;
    
    // Sample gradient at center and 6 neighbors
    float3 gradCenter = computeGradient(center, gradientEpsilon, ...);
    
    float gradientVariance = 0.0f;
    float3 neighbors[6] = {
        center + (float3)(gradientEpsilon, 0, 0),
        center - (float3)(gradientEpsilon, 0, 0),
        // ... Y and Z offsets
    };
    
    for (int i = 0; i < 6; i++)
    {
        float3 gradNeighbor = computeGradient(neighbors[i], gradientEpsilon, ...);
        float3 diff = gradCenter - gradNeighbor;
        gradientVariance += dot(diff, diff);
    }
    
    curvatureMetrics[leafId] = gradientVariance / 6.0f;
}
```

**Refinement Strategy:**
1. Evaluate curvature/feature detection on all current leaves
2. Mark high-curvature leaves for subdivision
3. Subdivide marked leaves (create 8 children)
4. Repeat until quality threshold met or max depth reached

### Phase 3: High-Precision Zero-Crossing Refinement

Use the existing `refineZeroCrossings` kernel iteratively:

```cpp
// After octree construction, refine Hermite sample positions
void refineHermitePositions()
{
    // Collect all edge zero-crossings from leaf nodes
    std::vector<EdgeCrossing> crossings = gatherLeafEdges();
    
    // Initial positions from linear interpolation (fast)
    std::vector<float3> positions = linearInterpolate(crossings);
    
    // GPU: Iterative refinement (3-5 iterations typical)
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        refineZeroCrossingsGPU(
            crossings,           // Edge endpoints + values
            positions,           // Current refined positions
            10,                  // Max bisection iterations per edge
            1e-6f                // Convergence tolerance
        );
    }
    
    // Now use refined positions for QEF vertex placement
    solveQEFforVertices(positions);
}
```

## Advantages Over Fixed Grid

### 1. Arbitrary Resolution
- No pre-determined grid size
- Memory grows only where surface exists
- Can achieve micron-level detail in specific regions without global cost

### 2. Adaptive Detail
- Flat regions: Large cells, few queries
- High-curvature regions (spheres, fillets): Small cells, dense sampling
- Sharp features: Automatic refinement to capture edges

### 3. Better Quality Per Cost
- Fixed grid 257³ = 17M samples (mostly wasted in empty space)
- Hierarchical: 10K-100K queries total, all meaningful
- Result: 100x fewer queries, better quality

### 4. Multiple Refinement Passes
```
Pass 1 (Coarse):   Depth 0-4, ~1K queries    → Find surface regions
Pass 2 (Medium):   Depth 5-7, ~10K queries   → Capture major features  
Pass 3 (Fine):     Depth 8-9, ~50K queries   → Refine high-curvature
Pass 4 (Polish):   Iterative, ~20K queries   → Zero-crossing bisection
```

## OpenCL Kernel Organization

### Core Kernels (Priority Order)

1. **`evaluateOctreeLevel`** - Parallel corner evaluation
   - Input: Array of bounding boxes (all nodes at depth N)
   - Output: 8 SDF values per node
   - Work items: One per octree node
   - Optimization: Coalesce corner queries, share edge values between siblings

2. **`detectIntersections`** - Sign change detection + subdivision decision
   - Input: Corner values array
   - Output: Subdivision flags (0/1 per node)
   - Work items: One per node
   - Fast: Just arithmetic, no SDF queries

3. **`estimateCurvature`** - Feature detection for adaptive refinement
   - Input: Leaf node positions
   - Output: Curvature metric per leaf
   - Work items: One per leaf node
   - Cost: 7 gradient evaluations = 42 SDF queries per leaf

4. **`refineZeroCrossings`** - Already implemented! Bisection refinement
   - Input: Edge endpoints + initial crossing positions
   - Output: Refined crossing positions
   - Work items: One per edge crossing
   - Iterative: 5-10 bisection steps per edge

5. **`batchGradientEvaluation`** - Batched gradient computation
   - Input: Positions array
   - Output: Gradients (for QEF/normal calculation)
   - Work items: One per position
   - Reuse existing `sampleHermite` kernel

### Optional Advanced Kernels

6. **`shareCornerValues`** - Exploit octree neighbor coherency
   - Adjacent nodes share corner values → deduplicate queries
   - Can reduce queries by ~4x with proper indexing

7. **`compactLeafData`** - Stream compaction
   - Remove non-intersecting nodes
   - Pack leaf data for efficient memory access

## CPU-GPU Workflow

```cpp
class HierarchicalDualContouring
{
public:
    struct Config
    {
        size_t maxDepth{9};                    // Deep refinement possible
        size_t initialDepth{4};                // Quick coarse structure
        float curvatureThreshold{0.3f};        // Adaptive refinement trigger
        size_t refinementIterations{3};        // Iterative improvement passes
        float zeroCrossingTolerance{1e-5f};    // Bisection convergence
        bool enableGradientCaching{true};      // Cache repeated gradient queries
    };
    
    void execute()
    {
        // Phase 1: Coarse octree (breadth-first, level-by-level)
        buildInitialOctree();  // Depth 0 → initialDepth
        
        // Phase 2: Feature-adaptive refinement (iterative)
        for (size_t pass = 0; pass < config.refinementIterations; ++pass)
        {
            identifyHighCurvatureLeaves();    // GPU kernel
            subdivideMarkedLeaves();          // CPU + GPU
        }
        
        // Phase 3: Zero-crossing refinement
        gatherEdgeCrossings();                // CPU
        refineHermitePositions();             // GPU iterative bisection
        
        // Phase 4: QEF solve and mesh generation
        solveQEFVertices();                   // Could be GPU or CPU
        extractDualMesh();                    // CPU traversal
    }
    
private:
    void buildInitialOctree()
    {
        for (size_t depth = 0; depth < config.initialDepth; ++depth)
        {
            auto& level = m_levels[depth];
            
            // Upload node bounds to GPU
            cl::Buffer nodeBuffer = uploadNodeBounds(level.nodes);
            cl::Buffer cornerBuffer(context, CL_MEM_WRITE_ONLY, 
                                    level.nodes.size() * 8 * sizeof(float));
            
            // GPU: Evaluate all corners in parallel
            m_kernelEvaluateLevel.setArg(0, nodeBuffer);
            m_kernelEvaluateLevel.setArg(1, cornerBuffer);
            queue.enqueueNDRangeKernel(m_kernelEvaluateLevel, 
                                       cl::NDRange(level.nodes.size()));
            
            // Download results
            std::vector<float> cornerValues(level.nodes.size() * 8);
            queue.enqueueReadBuffer(cornerBuffer, CL_TRUE, 0, 
                                   cornerValues.size() * sizeof(float),
                                   cornerValues.data());
            
            // GPU: Detect intersections
            cl::Buffer subdivisionBuffer(context, CL_MEM_WRITE_ONLY,
                                         level.nodes.size());
            m_kernelDetectIntersections.setArg(0, cornerBuffer);
            m_kernelDetectIntersections.setArg(1, subdivisionBuffer);
            queue.enqueueNDRangeKernel(m_kernelDetectIntersections,
                                       cl::NDRange(level.nodes.size()));
            
            // Download subdivision decisions
            std::vector<uint8_t> subdivFlags(level.nodes.size());
            queue.enqueueReadBuffer(subdivisionBuffer, CL_TRUE, 0,
                                   subdivFlags.size(), subdivFlags.data());
            
            // CPU: Create children for next level
            createChildLevel(level, subdivFlags, m_levels[depth + 1]);
        }
    }
    
    void identifyHighCurvatureLeaves()
    {
        // Get all current leaf nodes
        std::vector<OctreeNode*> leaves = collectLeaves();
        
        // GPU: Estimate curvature for each leaf
        cl::Buffer leafPosBuffer = uploadLeafPositions(leaves);
        cl::Buffer curvatureBuffer(context, CL_MEM_WRITE_ONLY,
                                   leaves.size() * sizeof(float));
        
        m_kernelEstimateCurvature.setArg(0, leafPosBuffer);
        m_kernelEstimateCurvature.setArg(1, curvatureBuffer);
        queue.enqueueNDRangeKernel(m_kernelEstimateCurvature,
                                   cl::NDRange(leaves.size()));
        
        // Download curvature metrics
        std::vector<float> curvatures(leaves.size());
        queue.enqueueReadBuffer(curvatureBuffer, CL_TRUE, 0,
                               curvatures.size() * sizeof(float),
                               curvatures.data());
        
        // Mark high-curvature leaves for subdivision
        for (size_t i = 0; i < leaves.size(); ++i)
        {
            if (curvatures[i] > config.curvatureThreshold &&
                leaves[i]->depth < config.maxDepth)
            {
                leaves[i]->needsRefinement = true;
            }
        }
    }
};
```

## Memory Efficiency

### Corner Value Sharing
```
Adjacent octree nodes share corners:
- Node (0,0,0) and Node (1,0,0) share 4 corners
- Sibling nodes share 1 corner (center of parent)

Strategy:
1. Assign global corner index: Hash(x,y,z,depth) → cornerID
2. Build corner→value cache (GPU hashmap or CPU std::unordered_map)
3. Query cache before evaluating SDF
4. Potential reduction: 8 queries/node → 2-3 unique queries/node
```

### Progressive Level-of-Detail
```cpp
// Start coarse, refine only visible/important regions
struct LODStrategy
{
    BoundingBox cameraFrustum;
    std::vector<BoundingBox> highDetailRegions;
    
    bool shouldRefine(OctreeNode const& node)
    {
        // Don't refine outside camera view
        if (!intersects(node.bounds, cameraFrustum))
            return false;
            
        // Always refine near high-detail regions
        for (auto& region : highDetailRegions)
        {
            if (intersects(node.bounds, region))
                return true;
        }
        
        // Distance-based LOD
        float distanceToCamera = distance(node.bounds.center(), cameraPos);
        size_t maxDepthAtDistance = computeLODDepth(distanceToCamera);
        return node.depth < maxDepthAtDistance;
    }
};
```

## Quality Presets (Revised)

```cpp
enum class HierarchicalQuality
{
    Draft,      // Depth 5, no refinement passes
    Balanced,   // Depth 7, 1 refinement pass
    Fine,       // Depth 8, 2 refinement passes
    UltraFine   // Depth 9, 3 refinement passes + zero-crossing polish
};

void applyPreset(HierarchicalQuality preset)
{
    switch (preset)
    {
    case Draft:
        config.initialDepth = 5;
        config.maxDepth = 5;
        config.refinementIterations = 0;
        config.curvatureThreshold = 1.0f;  // Disable adaptive
        break;
        
    case Balanced:
        config.initialDepth = 5;
        config.maxDepth = 7;
        config.refinementIterations = 1;
        config.curvatureThreshold = 0.4f;
        break;
        
    case Fine:
        config.initialDepth = 6;
        config.maxDepth = 8;
        config.refinementIterations = 2;
        config.curvatureThreshold = 0.25f;
        break;
        
    case UltraFine:
        config.initialDepth = 7;
        config.maxDepth = 9;
        config.refinementIterations = 3;
        config.curvatureThreshold = 0.15f;
        config.zeroCrossingTolerance = 1e-6f;
        break;
    }
}
```

## Performance Expectations

### Typical Gyroid Model (50mm cube)

**Current Fixed Grid (129³)**:
- SDF evaluations: 2,146,689 (all points, 90% wasted)
- Memory: ~8 MB grid
- Time: ~200ms (GPU) or ~2000ms (CPU)
- Quality: Mediocre (stairstepping visible)

**Hierarchical Approach (Balanced preset)**:
- Depth 7, adaptive refinement
- SDF evaluations: ~50,000 (only near surface)
- Corner value queries: ~8,000 nodes × 2 unique/node = ~16,000
- Curvature refinement: ~2,000 leaves × 42 queries = ~84,000
- Total queries: ~100,000 (20× fewer than fixed grid!)
- Memory: ~500 KB octree structure
- Time: ~150ms (GPU, level-by-level parallelism)
- Quality: Excellent (adaptive detail everywhere)

### GPU Utilization

```
Level 0: 1 node       → 1 work item (instant)
Level 1: 8 nodes      → 8 work items (instant)
Level 2: 64 nodes     → 64 work items (instant)
Level 3: 512 nodes    → 512 work items (good)
Level 4: 4,096 nodes  → 4,096 work items (excellent)
Level 5: ~8,000 nodes → 8,000 work items (peak parallelism)
```

From level 4 onwards, GPU is fully saturated. Levels 0-3 complete in microseconds.

## Implementation Roadmap

### Phase 1: Core Infrastructure (Week 1)
- [ ] `HierarchicalOctreeNode` structure
- [ ] Level-by-level traversal CPU orchestration
- [ ] `evaluateOctreeLevel` kernel
- [ ] `detectIntersections` kernel
- [ ] Basic subdivision logic

### Phase 2: Adaptive Refinement (Week 2)
- [ ] `estimateCurvature` kernel
- [ ] Iterative refinement passes
- [ ] Corner value caching/deduplication
- [ ] Leaf node management

### Phase 3: High-Precision Finishing (Week 3)
- [ ] Integrate existing `refineZeroCrossings` kernel
- [ ] Iterative zero-crossing bisection
- [ ] QEF solve with refined samples
- [ ] Mesh extraction from hierarchical octree

### Phase 4: Optimization (Week 4)
- [ ] Memory coalescing for corner queries
- [ ] Stream compaction for active nodes
- [ ] Gradient caching
- [ ] Performance profiling and tuning

### Phase 5: Quality Presets & UI (Week 5)
- [ ] Quality preset system
- [ ] Progress reporting (level-by-level)
- [ ] Statistics (query count, tree depth, memory usage)
- [ ] Comparison benchmarks vs fixed grid

## Key Insights

1. **Level-by-level is GPU-friendly**: Each level processes hundreds/thousands of nodes in parallel
2. **Adaptive = efficient**: Only query SDF where surface exists
3. **Iterative refinement**: Multiple cheap passes beat one expensive pass
4. **Reuse existing kernels**: `refineZeroCrossings` and `sampleHermite` already handle hard parts
5. **Corner sharing**: 4-8× reduction in redundant queries

## Risk Mitigation

**Risk**: CPU-GPU sync overhead between levels
**Mitigation**: Batch multiple depths into single GPU dispatch when possible, pipeline CPU work

**Risk**: Cache coherency for corner values
**Mitigation**: Use spatial hashing with Morton codes (already familiar from existing code)

**Risk**: Irregular memory access patterns
**Mitigation**: Sort nodes by spatial location before GPU dispatch, use local memory for hot data

**Risk**: QEF solve quality with adaptive sampling
**Mitigation**: Ensure adequate samples (min 3-4 crossings) before QEF, fallback to centroid if underconstrained

## Conclusion

This hierarchical approach fundamentally fixes the quality issues:
- **No pre-sampling artifacts** - query SDF at exact needed positions
- **Adaptive detail** - automatically refines high-curvature regions (your sphere issue)
- **Efficient** - 10-100× fewer queries than fixed grid
- **Flexible** - easy to tune quality vs performance
- **GPU-accelerated** - level-by-level parallelism keeps GPU busy

The key insight: Stop thinking "voxel grid", start thinking "hierarchical spatial subdivision with lazy evaluation".
