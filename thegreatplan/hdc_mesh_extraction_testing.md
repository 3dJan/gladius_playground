# Hierarchical Dual Contouring Mesh Extraction Testing Plan

## Goals

- Ensure hierarchical dual contouring (HDC) produces **valid, high-quality meshes** that pass ADMesh validation without relying on post-fixups.
- **Disable vertex projection** during tests so we validate the core extraction pipeline rather than the projection helper.
- Introduce **unit tests for each major step** of the mesh extraction pipeline.
- Optionally **refactor** HDC extraction to be more modular and testable, without increasing memory footprint or breaking GPU-first behavior.

## Constraints and Priorities

- **GPU path is more important** than CPU; tests and refactorings must keep GPU usage healthy.
- **Memory footprint must stay small**:
  - Continue to use `releaseCornerValues()` and `releaseHermiteData()` to drop large arrays when no longer needed.
  - Avoid duplicating large data (e.g., node arrays, SDF volumes).
- Do not change the public API of `HierarchicalOctreeBuilder` or the STL exporter unless absolutely necessary.

---

## 1. Disable Vertex Projection for Tests

### 1.1. Where projection is controlled

- Projection is performed in `HierarchicalOctreeBuilder::extractMesh` (in `HierarchicalDualContouring.cpp`):
  - Uses `m_config.projectVerticesToSurface && m_config.enableGpuAcceleration` guard.
  - Calls into `SlicerProgram::adoptVertexOfMeshToSurface` to project vertices onto the implicit surface.
- Test path: `tests/unittests/HierarchicalDC_STLExport_tests.cpp` via `io::HierarchicalDualContouringOptions` and `HierarchicalDualContouringStlExporter`.

### 1.2. Test-only configuration change

**Plan**

- In the HDC STL export tests, explicitly disable vertex projection:
  - After `options.applyPreset()`, add `options.config.projectVerticesToSurface = false;`.
- Apply this in all HDC exporter tests that use ADMesh validation:
  - `FullStlExport_PassesAdmeshAnalysis` baseline test.
  - Parameterized `HierarchicalDC_STL_CombinationTest.ExportConfigurations_AdmeshAnalysis` scenarios.

This keeps runtime behavior unchanged for the application (projection still runs), but isolates test expectations to the core extraction pipeline.

---

## 2. Decompose Mesh Extraction into Testable Steps

Current `HierarchicalOctreeBuilder::extractMesh` performs several responsibilities in one method:

1. Collect leaf indices (`getLeafIndices`).
2. Build a node-to-vertex index map from `OctreeNode::vertexPosition`.
3. Optionally project vertices to the surface (GPU).
4. Infer neighbor cells via `findLeafAt` / `findNeighbor`.
5. Traverse edges and emit triangles (quads split into two tris) into `outIndices`.

### 2.1. Target structure for modularity

Without changing the public signature, internally split extraction into helpers:

- `buildVertexIndexMap(...)`  
  Input: leaf indices and `m_nodes`.  
  Output: `nodeToVertexIndex` and `outVertices`.

- `projectVerticesIfEnabled(...)`  
  Encapsulate the projection logic and its error handling.

- `findLeafAtPoint(...)` / `findNeighborCell(...)`  
  Move lambdas into small private member functions for reuse and direct testing.

- `emitTopologyFromEdges(...)`  
  Input: `leafIndices`, `nodeToVertexIndex`, edge table;  
  Output: `outIndices`.  
  This function is responsible for:
  - Skipping non-intersecting leaves.
  - Using consistent edge ordering and vertex winding.
  - Handling missing neighbors gracefully (e.g., not emitting incomplete polygons).

**Benefits**

- Each step can be unit-tested via a dedicated test fixture.
- No new large persistent state is introduced; we mostly lift existing lambdas to member functions.

---

## 3. Unit Tests for Each Extraction Stage

Tests will live in `tests/unittests/HierarchicalDC_STLExport_tests.cpp` or a new file, e.g., `HierarchicalDC_ExtractionStep_tests.cpp`, under namespace `gladius_tests::hierarchical_dc_mesh`.

### 3.1. Setup helper for HDC builder

Introduce a small test helper function to construct a `HierarchicalOctreeBuilder` in a controlled environment:

- Use existing `Document` loading from `ImplicitGyroid.3mf` (as current tests do) to avoid duplicating SDF setup.
- Configure `HierarchicalConfig` with:
  - `enableGpuAcceleration = true` (GPU-preferred).
  - Preset (e.g., `Balanced`) for deterministic behavior.
- Build the octree via `builder.buildOctree(bbox)`.

### 3.2. Test: Vertex index map creation

**Objective:** Ensure `buildVertexIndexMap` sees all intersecting leaves with `hasVertex == true` and creates a dense, contiguous vertex buffer.

Assertions:

- `outVertices.size() == nodeToVertexIndex.size()`.
- All indices in `nodeToVertexIndex` are `< outVertices.size()`.
- No `nodeToVertexIndex` entry for leaves without `hasVertex`.

### 3.3. Test: Topology emission (edge processing)

**Objective:** Given a built octree with vertices, ensure the topology step produces a reasonable triangle mesh.

Assertions:

- `outIndices.size() % 3 == 0` (triangles only).
- `outIndices` references only existing vertices (`< outVertices.size()`).
- No degenerate triangles where any two vertex indices are equal.

Optional additional checks:

- Count of triangles is above a minimum threshold for the gyroid model.
- Basic normal consistency: compute face normals and ensure the majority are outward consistent (heuristic).

### 3.4. Test: Projection disabled behavior

**Objective:** Confirm that disabling projection does not alter vertex topology.

Scenario:

1. Build octree and solve QEF vertices.
2. Call `extractMesh` with `projectVerticesToSurface = false` and record `verticesA`, `indicesA`.
3. (If we keep projection enabled in general runtime) call `extractMesh` again with projection enabled and record `verticesB`, `indicesB`.

Assertions:

- `indicesA == indicesB` (topology unaffected by pure projection stage).  
  *If this is not currently true, it exposes a bug worth fixing later.*
- For tests, ensure `projectVerticesToSurface` is always set to false as per section 1.

### 3.5. Test: Neighbor-finding across depth changes

**Objective:** Stress `findNeighborCell`/`findLeafAtPoint` near octree depth transitions.

Approach:

- Synthetic setup if feasible: create a small custom SDF or a synthetic node layout that guarantees a refined cell adjacent to a coarser one.
- Alternatively, use the gyroid model and focus on a small region by inspecting a subset of leaf nodes.

Assertions:

- For an intersecting leaf with a sign-change edge, its neighbor indices (n0/n1/n2) resolve to leaves that either have vertices or are explicitly absent, but do not point back into non-leaf nodes.
- No infinite loops or invalid indices from `findLeafAtPoint`.

(If synthetic fixture proves too intrusive, this part can be postponed or implemented with a small custom SDF helper later.)

### 3.6. Test: GPU triangle orientation metrics

**Objective:** Ensure the dedicated OpenCL orientation kernel executes during extraction, flips badly oriented triangles, and reports sane metrics via `ConstructionStats::orientationMetrics`.

Assertions:

- `orientationMetrics.triangleCount == indices.size() / 3` after extraction.
- `orientationMetrics.flippedCount <= orientationMetrics.triangleCount` and `fallbackCount <= triangleCount`.
- Recorded qualities stay inside $[0, 1]$ with `averageQuality >= 0.5` for the gyroid fixture (tunable once empirical data is collected).
- When `orientTrianglesOnGpu` is disabled, the stats struct resets to zeros so regression tests can tell whether the kernel actually ran.

Test flow:

1. Build the gyroid octree with GPU acceleration and `orientTrianglesOnGpu = true` while keeping projection disabled.
2. Call `extractMesh`, inspect stats, and assert constraints above.
3. Repeat with `orientTrianglesOnGpu = false` to ensure stats zero out, proving the GPU pass is gated correctly.

---

## 4. Additional Refactoring Opportunities (Optional)

Future work to improve testability and mesh quality without sacrificing memory or GPU emphasis:

1. **Expose a lightweight stats struct for extraction** (number of quads, triangles, degenerate edges rejected) to assert against in unit tests.
2. **Add a debug-only consistency check** (behind a macro) to verify that each emitted triangle corresponds to exactly one dual contouring edge quad.
3. **Instrument GPU fallbacks**:
   - Track when `evaluateCornersGPU` / `estimateCurvatureGPU` fail and CPU fallback is used.
   - Expose this via `ConstructionStats` so tests can assert that the GPU path is actually exercised.
4. **Clamp projection displacements** (when re-enabling projection checks): bound projected vertices to stay within their leaf`s bounding box to reduce self-intersection risk.

5. **Expose orientation metrics in exporter logs/tests** so ADMesh regressions can correlate repairs with kernel quality (hook into `io::HierarchicalDualContouringStlExporter`).

---

## 5. Validation Plan

After implementing the test configuration change and new unit tests:

1. **Rebuild** using the existing preset:

   - `cmake --build out/build/linux-releaseWithDebug --parallel 8`

2. **Run targeted tests**:

   - `ctest --preset ReleaseWithDebug -V -R HierarchicalDualContouringVariants`
   - `ctest --preset ReleaseWithDebug -V -R HierarchicalDC_Extraction`

3. **Inspect ADMesh output** from the export tests to confirm:

   - STL file is written and contains triangles.
   - No facets added/removed.
   - No backwards edges.
   - No normals fixed.

4. Iteratively refine extraction logic if any of the new step-level tests reveal inconsistencies (especially in neighbor-finding and topology emission).

5. Capture `orientationMetrics` in the STL export fixtures and flag any run where `flippedCount` grows or `averageQuality` drops below the agreed threshold, preventing regressions in triangle winding quality.

This plan keeps vertex projection out of the equation for tests, gives us high-granularity coverage of the mesh extraction pipeline, and sets up a safe path to future refactors that improve GPU-centric mesh quality while respecting memory constraints.
