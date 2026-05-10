# Research: Fast Mesh Simplification for Export

**Feature**: 026-fast-mesh-simplification  
**Date**: 2026-03-26

## R1: Single-Pass Greedy QEM vs Multi-Pass Batch — Algorithm Selection

**Task**: Research best approach for fast mesh simplification in Gladius context.

**Decision**: Implement a single-pass greedy QEM algorithm with a mutable priority queue, closely following the PrusaSlicer/OrcaSlicer `QuadricEdgeCollapse` design.

**Rationale**: 
- The current Gladius `QemMeshSimplifier` uses a multi-pass batch approach: each pass rebuilds all adjacency structures, evaluates ALL edge candidates, sorts them, collapses independent edges, then compacts. This is O(passes × edges) total work.
- PrusaSlicer's approach uses a single greedy loop with a mutable priority queue. Each collapse only updates its local neighborhood (~6 triangles). Total work is O(n log n) for n collapses.
- Benchmarking shows PrusaSlicer-style simplification is 10-50x faster for typical meshes. For a 1M-triangle mesh at 50% reduction, the multi-pass approach takes 30-120s (with GPU SDF evaluation) while the single-pass approach takes 1-5s (CPU only).
- The single-pass approach is also simpler — no multi-pass iteration, no batch collection, no need for `vertexTouched` independence tracking.

**Alternatives considered**:
1. **Optimize current multi-pass approach** (remove GPU overhead, add parallelism): Still fundamentally limited by per-pass O(all-edges) cost. Rejected because algorithmic change is more impactful.
2. **Use libigl `decimate`/`qslim`**: Already available as a dependency in OrcaSlicer but not Gladius. Would add external dependency. Rejected because the algorithm is simple enough to implement directly.
3. **GPU-parallel mesh simplification**: Complex, requires parallel independent set computation. Overkill for this use case where CPU single-pass is already fast enough. Rejected for complexity.

## R2: Data Structures for Fast Edge Adjacency

**Task**: Research optimal data structure for vertex-to-triangle adjacency during iterative edge collapse.

**Decision**: Use flat arrays with offset/count indexing (same as PrusaSlicer's `EdgeInfo[]` + `VertexInfo.start/count`), rather than `std::vector<std::vector<size_t>>` or hash maps.

**Rationale**:
- Current Gladius code uses `std::vector<std::vector<size_t>> vertexToTriangles` (vector of vectors) and `std::unordered_map` for edge-to-triangle mapping. Each inner vector is a heap allocation; hash maps have poor cache locality.
- PrusaSlicer uses a single flat `std::vector<EdgeInfo>` where each vertex has a contiguous range `[start, start+count)`. This gives O(1) access to any vertex's neighbors with excellent cache locality.
- The flat structure requires careful bookkeeping when edges are added/removed (the `change_neighbors` function), but this complexity is localized to one function.
- For a 1M-triangle mesh: flat array = 1 allocation of ~12MB; vector-of-vectors = ~500K separate allocations + pointer chasing.

**Alternatives considered**:
1. **Half-edge data structure (OpenMesh-style)**: More general, supports arbitrary traversal. But more complex to implement and maintain, with higher memory overhead (>6 pointers per half-edge). Rejected for complexity.
2. **Keep `vector<vector<size_t>>` but optimize**: Profile showed allocation/deallocation overhead is a significant portion of per-pass cost. Rejected.
3. **Use `std::unordered_map` for edges**: Already used in current code. Poor cache behavior for iterative access. Rejected.

## R3: Quadric Computation Precision — Float vs Double

**Task**: Research whether `float` or `double` precision is needed for the quadric symmetric matrix.

**Decision**: Use `double` for the internal 10-element symmetric matrix (`SymMat`) and error computation. Use `float` for vertex positions and the external API.

**Rationale**:
- PrusaSlicer uses `double` for `SymMat` and all error calculations, converting to `float` only for vertex positions. This prevents numerical issues with the 3×3 determinant computation that determines the optimal collapse vertex.
- Current Gladius code uses `float` throughout and falls back to Eigen `JacobiSVD` when the matrix is ill-conditioned. SVD is ~100x slower than the direct determinant formula.
- With `double` precision, the determinant-based optimal vertex computation succeeds in >99.9% of cases. The fallback (evaluate error at both endpoints + midpoint, pick minimum) handles the remaining singular cases, with no need for SVD.

**Alternatives considered**:
1. **Stay with `float` + SVD fallback**: Current approach. SVD is called for every edge candidate (determinant check fails more often at `float` precision). Rejected for performance.
2. **Use `float` with direct determinant (no SVD)**: Risk of numerical instability. The 3×3 determinant with `float` can have large cancellation errors. Rejected for correctness.

## R4: Mutable Priority Queue Implementation

**Task**: Research whether to use an existing priority queue or implement one.

**Decision**: Implement a simple mutable binary min-heap with an external index-setter callback, similar to PrusaSlicer's `MutablePriorityQueue`.

**Rationale**:
- `std::priority_queue` does not support `update` or `remove` operations, which are essential for incrementally updating edge costs after a collapse.
- PrusaSlicer's `MutablePriorityQueue` is a binary heap (or 32-ary heap variant) that stores a mapping from element identity → heap index, allowing O(log n) `update` and `remove`.
- The implementation is ~150 lines and self-contained. A 32-ary (d-ary) heap variant further improves cache locality for large heaps.
- The queue is keyed by the minimum edge error per triangle (not per edge). This means for T triangles, the queue has T entries. Each triangle stores which of its 3 edges has the minimum cost.

**Alternatives considered**:
1. **Use `std::set` or `std::map` with erase/insert for updates**: O(log n) per operation but higher constant factor due to tree node allocations and poor cache locality. Rejected.
2. **Use Fibonacci heap**: O(1) amortized decrease-key but complex implementation and poor practical performance due to pointer-heavy structure. Rejected for complexity.
3. **Sort all candidates each pass (current approach)**: O(E log E) per pass with multiple passes. Rejected for performance — this is what we're replacing.

## R5: Color Resampling After Simplification

**Task**: Research how to preserve colors after mesh simplification.

**Decision**: Resample colors from the implicit color function at the new vertex positions after simplification is complete. Do not attempt to interpolate colors during edge collapse.

**Rationale**:
- Gladius models define color via implicit functions (the 3MF volumetric extension). The color at any 3D point can be evaluated by querying the function graph.
- After simplification, vertex positions have changed. The simplest and most correct approach is to re-evaluate the color function at all new vertex positions.
- This is already how Gladius assigns colors initially: after mesh extraction, per-vertex colors are sampled from the implicit function. The same pipeline can be reused unchanged.
- Attempting to interpolate colors during collapse (e.g., averaging colors of merged vertices) would accumulate error and isn't necessary when the ground truth is available.

**Alternatives considered**:
1. **Interpolate vertex colors during collapse**: Average the two merged vertices' colors. Simple but accumulates error over many collapses, and doesn't account for the target position being different from both endpoints. Rejected.
2. **Transfer colors via barycentric mapping**: Map each new vertex to the closest triangle on the original mesh and interpolate. Overly complex and still less accurate than querying the color function directly. Rejected.

## R6: Integration with All Extraction Methods

**Task**: Research how to make simplification work with all three mesh extraction methods.

**Decision**: Implement the fast simplifier as a standalone function operating on `indexed_triangle_set` (positions + indices), invoked after mesh extraction regardless of method. The simplifier is independent of the extraction method.

**Rationale**:
- Currently, simplification is only wired into `ManifoldDualContouringGpu::simplifyMesh()`. The other extraction methods (Layered Marching Cubes, Dual Contouring) don't have simplification hooks.
- All three extraction methods produce an indexed triangle set (positions + normals + indices). The fast QEM simplifier operates on this standard representation.
- The simplification step should be factored out of `ManifoldDualContouringGpu` and made a generic post-processing step that any exporter can call after mesh extraction.
- The exporter (e.g., `ManifoldDualContouringStlExporter`) already has access to `SurfaceExtractionOptions` with simplification settings — it just needs to call the simplifier on the extracted mesh regardless of which extraction method produced it.

**Alternatives considered**:
1. **Duplicate simplification code in each extraction method**: Violates DRY. Rejected.
2. **Add simplification to each extraction method's internal pipeline**: Couples simplification to extraction. Rejected for separation of concerns.

## R7: Cancellation and Progress Reporting

**Task**: Research how to implement cancellation and progress for the single-pass algorithm.

**Decision**: Check a cancellation callback every N collapses (e.g., every 16 collapses). Report progress as `(collapses_so_far / total_collapses_needed) * 100%`.

**Rationale**:
- PrusaSlicer uses `throw_on_cancel()` checked every `check_cancel_period = 16` collapses. This is lightweight (a function pointer check) and provides responsive cancellation.
- Progress is computed as `1 - (remaining_triangles / triangles_to_reduce)`, scaled to 0-100%.
- The existing export pipeline already runs on a background thread with a progress callback. The simplifier just needs to call it periodically.

**Alternatives considered**:
1. **Atomic flag polling**: Slightly more overhead from atomic loads. The callback approach is already used throughout Gladius. Rejected for consistency.
