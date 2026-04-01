# Morton Quadtree Slice Extraction - Implementation Plan

## Objective
Implement a Morton-encoded quadtree-based slice extraction system that reliably detects 200 μm thin walls while optimizing OpenCL buffer allocation and memory usage.

## Background
- Current slice extraction uses fixed mip-map hierarchy (256² → 1024² → 4096² → 8000²)
- Thin wall detection is implicit via branch thresholds, which may miss 200 μm features
- 3D surface extraction already has Morton octree with explicit `minFeatureSize` protection
- OpenCL buffer allocation is expensive and must be optimized

## Key Requirements
1. **Reliable 200 μm thin wall detection** - explicit feature size constraint
2. **Memory efficiency** - sparse allocation only where surface exists
3. **Performance** - minimize OpenCL buffer allocations, reuse buffers
4. **Watertight contours** - Morton cell boundaries ensure consistency
5. **Testability** - benchmark tests for performance validation

## Architecture

### Core Components

#### 1. MortonQuadtree (CPU-side)
```cpp
namespace gladius::slicer
{
    struct QuadNode
    {
        std::uint64_t mortonCode{0U};
        std::uint8_t depth{0U};
        bool isLeaf{true};
        bool isIntersecting{false};
        bool needsRefinement{false};
        
        std::array<float, 4> cornerValues{};
        std::uint8_t cornerSignMask{0U};
        
        std::array<std::size_t, 4> childIndices{};
        std::vector<EdgeCrossing2D> edgeCrossings;
    };
    
    class MortonQuadtree
    {
    public:
        void build(float minFeatureSize, BoundingBox2D const& bounds);
        void refineAdaptively(float curvatureThreshold);
        
    private:
        std::vector<QuadNode> m_nodes;
        std::unordered_map<std::uint64_t, std::size_t> m_mortonToIndex;
    };
}
```

#### 2. MortonSlicerProgram (GPU-side)
```cpp
class MortonSlicerProgram : public ProgramBase
{
public:
    void renderSlice(
        Primitives const& lines,
        cl_float isoValue,
        cl_float z_mm,
        float minFeatureSize
    );
    
private:
    // Buffer pool for reuse across slices
    cl::Buffer m_cornerValueBuffer;      // Reusable corner value buffer
    cl::Buffer m_nodeBuffer;             // Quadtree nodes
    cl::Buffer m_edgeCrossingBuffer;     // Edge crossing data
    
    bool m_buffersAllocated{false};
    std::size_t m_allocatedNodeCapacity{0U};
    
    void ensureBuffers(std::size_t nodeCount);
};
```

#### 3. BufferPool (Optimization)
```cpp
class SliceBufferPool
{
public:
    cl::Buffer& acquireCornerBuffer(std::size_t size);
    cl::Buffer& acquireEdgeBuffer(std::size_t size);
    void releaseAll();  // Called after slice batch completes
    
private:
    std::unordered_map<std::size_t, cl::Buffer> m_cornerBuffers;
    std::unordered_map<std::size_t, cl::Buffer> m_edgeBuffers;
    bool m_enablePooling{true};
};
```

### OpenCL Kernels

#### 1. Corner Sampling Kernel
```opencl
__kernel void sampleQuadtreeCorners(
    __global QuadNode* nodes,
    __global float* cornerValues,
    const int numNodes,
    const float z_mm,
    // ... SDF payload arguments
);
```

#### 2. Adaptive Refinement Kernel
```opencl
__kernel void markRefinement(
    __global QuadNode* nodes,
    const int numNodes,
    const float minFeatureSize,
    const float curvatureThreshold
);
```

#### 3. Edge Crossing Kernel
```opencl
__kernel void computeEdgeCrossings(
    __global QuadNode* nodes,
    __global EdgeCrossing2D* crossings,
    const int numNodes,
    const float isoValue
);
```

## Implementation Phases

### Phase 1: Core Data Structures (Week 1)
**Files to create:**
- `gladius/src/slicer/MortonQuadtree.h`
- `gladius/src/slicer/MortonQuadtree.cpp`
- `gladius/src/slicer/QuadNode.h`

**Tasks:**
1. Implement 2D Morton encoding/decoding
2. Implement QuadNode structure with corner values
3. Implement quadtree construction from bounding box
4. Implement adaptive refinement logic with `minFeatureSize` constraint
5. Write unit tests for Morton encoding
6. Write unit tests for quadtree construction

**Tests:**
- `tests/MortonQuadtreeTest.cpp` - Morton encoding correctness
- `tests/QuadtreeConstructionTest.cpp` - Tree building and refinement

### Phase 2: GPU Integration (Week 2)
**Files to create:**
- `gladius/src/slicer/MortonSlicerProgram.h`
- `gladius/src/slicer/MortonSlicerProgram.cpp`
- `gladius/src/kernel/morton_slicer.cl`

**Tasks:**
1. Create MortonSlicerProgram class
2. Implement buffer pool with reuse strategy
3. Write OpenCL kernels for corner sampling
4. Write OpenCL kernels for edge crossing computation
5. Implement hybrid approach: mip-map → quadtree refinement
6. Write integration tests

**Tests:**
- `tests/MortonSlicerProgramTest.cpp` - GPU kernel correctness
- `tests/BufferPoolTest.cpp` - Buffer reuse performance

### Phase 3: Contour Extraction (Week 3)
**Files to modify:**
- `gladius/src/ContourExtractor.cpp` - Add Morton quadtree support
- `gladius/src/SlicerProgram.cpp` - Integrate Morton quadtree

**Tasks:**
1. Implement Marching Squares on Morton quadtree leaf nodes
2. Ensure watertight contours via Morton cell boundaries
3. Handle multi-vertex cells for complex configurations
4. Integrate with existing contour post-processing pipeline
5. Write end-to-end tests

**Tests:**
- `tests/ContourExtractionTest.cpp` - Watertight contour validation
- `tests/ThinWallTest.cpp` - 200 μm thin wall detection

### Phase 4: Performance Optimization (Week 4)
**Files to create:**
- `gladius/src/slicer/SliceBufferPool.h`
- `gladius/src/slicer/SliceBufferPool.cpp`

**Tasks:**
1. Implement buffer pooling with size-based caching
2. Implement lazy allocation strategy
3. Add buffer warm-up during initialization
4. Profile and optimize hot paths
5. Write benchmark tests

**Tests:**
- `tests/SlicerBenchmark.cpp` - Performance benchmarks
- `tests/MemoryUsageTest.cpp` - Memory footprint validation

### Phase 5: Integration & Testing (Week 5)
**Tasks:**
1. Integrate with UI (add `minFeatureSize` parameter to slice settings)
2. Update documentation
3. Write comprehensive test suite
4. Performance regression testing
5. Memory leak testing

## Performance Targets

| Metric | Current | Target | Improvement |
|--------|---------|--------|-------------|
| Memory (400mm²) | ~3.8 GB | ~200 MB | 95% reduction |
| Buffer allocation time | ~50ms per slice | ~5ms (pooled) | 90% reduction |
| Thin wall detection | Unreliable | 100% @ 200μm | Reliable |
| Slice time | ~200ms | ~150ms | 25% faster |

## Buffer Allocation Strategy

### Problem
OpenCL buffer allocation is expensive (~10-50ms per buffer). Current approach allocates full-resolution buffers for every slice.

### Solution: Three-Tier Buffer Pool

#### Tier 1: Persistent Buffers (allocated once)
- Corner value buffer (max 1M corners)
- Node buffer (max 100K nodes)
- Edge crossing buffer (max 500K crossings)

#### Tier 2: Size-Based Pool (reused within slice batch)
- Small buffers (< 1MB): pool of 10
- Medium buffers (1-10MB): pool of 5
- Large buffers (> 10MB): pool of 2

#### Tier 3: Lazy Allocation
- Allocate on first use
- Grow geometrically (2x when needed)
- Never shrink during slice batch

### Implementation
```cpp
class SliceBufferPool
{
public:
    cl::Buffer& acquireBuffer(std::size_t size, BufferType type)
    {
        if (!m_enablePooling)
        {
            return allocateNewBuffer(size);
        }
        
        // Find best-fit buffer from pool
        auto& pool = getPoolForSize(size, type);
        if (!pool.empty())
        {
            auto& buffer = pool.back();
            pool.pop_back();
            return buffer;
        }
        
        // Allocate new if pool empty
        return allocateNewBuffer(size);
    }
    
    void releaseBuffer(cl::Buffer& buffer, BufferType type)
    {
        if (m_enablePooling)
        {
            getPoolForBuffer(buffer, type).push_back(buffer);
        }
    }
    
private:
    std::array<std::vector<cl::Buffer>, 3> m_smallPools;
    std::array<std::vector<cl::Buffer>, 3> m_mediumPools;
    std::array<std::vector<cl::Buffer>, 3> m_largePools;
};
```

## Testing Strategy

### Unit Tests
1. **Morton Encoding**
   - Encode/decode roundtrip
   - Spatial locality verification
   - Edge cases (boundary coordinates)

2. **Quadtree Construction**
   - Initial construction from bounding box
   - Adaptive refinement logic
   - Thin wall protection (`minFeatureSize` constraint)

3. **Buffer Pool**
   - Acquire/release cycle
   - Pool size limits
   - Concurrent access (if needed)

### Integration Tests
1. **GPU Kernels**
   - Corner sampling correctness
   - Edge crossing computation
   - Multi-node processing

2. **Contour Extraction**
   - Watertight contour validation
   - Thin wall detection (200 μm)
   - Complex geometries (saddle points, multiple contours)

### Benchmark Tests
1. **Performance Benchmarks**
   ```cpp
   TEST_F(SlicerBenchmark, MeasureSliceTime)
   {
       auto start = std::chrono::high_resolution_clock::now();
       
       for (int i = 0; i < 100; ++i)
       {
           slicer.renderSlice(lines, 0.0f, z_mm, 0.2f);
       }
       
       auto end = std::chrono::high_resolution_clock::now();
       auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
       
       EXPECT_LT(duration.count() / 100, 150);  // < 150ms per slice
   }
   ```

2. **Memory Benchmarks**
   ```cpp
   TEST_F(MemoryUsageTest, MeasureMemoryFootprint)
   {
       auto before = getGPUMemoryUsage();
       
       MortonSlicerProgram slicer(context, resources);
       slicer.renderSlice(lines, 0.0f, z_mm, 0.2f);
       
       auto after = getGPUMemoryUsage();
       auto delta = after - before;
       
       EXPECT_LT(delta, 300 * 1024 * 1024);  // < 300 MB
   }
   ```

3. **Buffer Allocation Benchmarks**
   ```cpp
   TEST_F(BufferPoolBenchmark, MeasureAllocationTime)
   {
       // Without pooling
       auto start = std::chrono::high_resolution_clock::now();
       for (int i = 0; i < 100; ++i)
       {
           cl::Buffer buffer(context, CL_MEM_READ_WRITE, 1024 * 1024);
       }
       auto withoutPool = std::chrono::high_resolution_clock::now() - start;
       
       // With pooling
       SliceBufferPool pool(context);
       start = std::chrono::high_resolution_clock::now();
       for (int i = 0; i < 100; ++i)
       {
           auto& buffer = pool.acquireBuffer(1024 * 1024, BufferType::Corner);
           pool.releaseBuffer(buffer, BufferType::Corner);
       }
       auto withPool = std::chrono::high_resolution_clock::now() - start;
       
       EXPECT_LT(withPool.count(), withoutPool.count() / 5);  // 5x faster
   }
   ```

### Regression Tests
1. **Thin Wall Detection**
   - Create test model with 200 μm walls
   - Verify all walls are detected
   - Verify contours are watertight

2. **Memory Regression**
   - Monitor memory usage across releases
   - Alert if usage increases > 10%

3. **Performance Regression**
   - Track slice time across releases
   - Alert if time increases > 10%

## Risk Mitigation

### Risk 1: Morton Quadtree Overhead
**Mitigation:** Use hybrid approach - mip-map for coarse detection, quadtree only where needed

### Risk 2: Buffer Pool Memory Leak
**Mitigation:** Implement RAII wrappers, add leak detection in tests

### Risk 3: GPU Kernel Bugs
**Mitigation:** Extensive unit tests, reference CPU implementation for validation

### Risk 4: Performance Regression
**Mitigation:** Continuous benchmarking, performance alerts in CI

## Success Criteria

1. ✅ 200 μm thin walls detected reliably (100% in test suite)
2. ✅ Memory usage < 300 MB for 400mm² build area
3. ✅ Buffer allocation time < 5ms per slice (with pooling)
4. ✅ Slice time < 150ms average
5. ✅ All contours watertight (no gaps or overlaps)
6. ✅ All tests pass (unit, integration, benchmark)

## Next Steps

1. Start with Phase 1: Core data structures
2. Implement Morton encoding and quadtree construction
3. Write unit tests for correctness
4. Proceed to GPU integration once core is stable
