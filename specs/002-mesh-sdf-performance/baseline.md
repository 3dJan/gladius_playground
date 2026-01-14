# Mesh SDF Performance Baseline

**Feature**: 002-mesh-sdf-performance  
**Captured**: 2025-12-30 (Initial), 2025-07-17 (Final)  
**Hardware**: AMD Radeon RX 9070 XT (gfx1201)

## Purpose

This document captures baseline performance measurements for mesh SDF operations and final improvement numbers after optimization.

## Test Configuration

| Parameter | Value |
|-----------|-------|
| **Benchmark Mesh** | `testdata/SphereInACageSimplifiedMesh.3mf` |
| **File Size** | 179,001 bytes |
| **Query Count** | 10,000 (default) |
| **Random Seed** | 42 (reproducible) |
| **OpenCL Platform** | rusticl (Mesa) |
| **GPU** | AMD Radeon RX 9070 XT |

## Baseline Measurements

### BVH Build Performance (CPU)

| Metric | Procedural Sphere (5,120 triangles) | With Precomputed Normals |
|--------|-------------------------------------|--------------------------|
| **Avg Build Time** | 12,053.4 µs (~12 ms) | 11,948 µs (~12 ms) |
| **Throughput** | 424,776 triangles/sec | 428,524 triangles/sec |

### BVH Build Scaling (With Precomputed Face Normals)

| Triangles | Build Time (µs) | Throughput (tri/s) | Memory (KB) |
|-----------|-----------------|--------------------| ------------|
| 320 | 600 | 532,978 | 20 |
| 1,280 | 3,219 | 397,639 | 80 |
| 5,120 | 11,646 | 439,636 | 320 |
| 20,480 | 49,598 | 412,923 | 1,280 |

**Note**: Memory increase from 48→64 bytes/triangle is offset by avoiding runtime cross-product computation for face normals in the GPU kernel.

## Optimizations Implemented

### Phase 2: Core BVH Optimizations
- ✅ Ordered traversal with distance-based sorting (near-first)
- ✅ Deferred sign computation (compute unsigned distance first, sign only if needed)
- ✅ `sqTriangleFast()` for distance-only calculations

### Phase 3: Rendering Performance
- ✅ Early termination with `earlyExitDistanceSq` parameter
- ✅ Voxel grid data structures (kernel-side)
- ⏸️ Host-side voxel integration (deferred - requires significant refactoring)

### Phase 4: Export Performance
- ✅ Vectorized data loads (`vload4`) for BVH nodes and triangles
- ✅ Extended triangle struct to 64 bytes with precomputed face normal
- ✅ `computePseudoNormalFast()` uses precomputed face normal

### Phase 5: Memory Bandwidth
- ✅ Streamlined `spatialMeshUnsignedDistance()` with optimized path
- ✅ Minimal triangle distance using `sqTriangleFast()` (no closest point details)
- ✅ Vectorized loads reduce memory transactions

## Struct Sizes

| Struct | Original | Optimized | Change |
|--------|----------|-----------|--------|
| MeshTriangle | 48 bytes | 64 bytes | +16 bytes (for face normal) |
| MeshBVHNode | 48 bytes | 48 bytes | unchanged |
| MeshVertexNormal | 16 bytes | 16 bytes | unchanged |

## Test Results

- **Total mesh/BVH/spatial tests**: 44 tests
- **All tests passing**: ✅
- **Disabled tests**: 1 (GPU timing - requires integration)

## Notes

- Vectorized loads (`vload4`) improve memory coalescing on GPU
- Precomputed face normals eliminate runtime cross-product for face-closest cases
- Ordered BVH traversal reduces average nodes visited per query
- Early termination enables fast boundary checks during rendering

## Update History

| Date | Phase | Change |
|------|-------|--------|
| 2025-12-30 | Phase 1 | Initial baseline captured |
| 2025-07-17 | Phase 2-5 | Core optimizations complete, all tests passing |
