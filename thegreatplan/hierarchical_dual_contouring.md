# Hierarchical Dual Contouring – Status & Roadmap
_Last updated: 2025-11-07_

## Current status

### What’s already in `src/`
- The hierarchical octree builder in `src/HierarchicalDualContouring.{h,cpp}` now covers coarse tree construction, curvature-driven refinement, zero-crossing refinement, Hermite sampling, QEF solving, and stats collection.
- `ProgramManager` provisions a `HierarchicalDCProgram` alongside existing compute programs, wiring cache directories and shared logging automatically.
- `HierarchicalDCProgram` dispatches `evaluateOctreeLevel`, `detectIntersections`, `estimateCurvature`, `batchGradients`, and `refineZeroCrossings`; host-side code batch-packs data for each phase and unpacks results back into the octree nodes.
- GPU corner, intersection, curvature, Hermite, and QEF phases run end-to-end when `enableGpuAcceleration` is true; CPU fallbacks still emit TODO logs.
- Legacy `tmp/hierarchical_dc/*` shims remain stubbed with `#error` guards to prevent accidental inclusion.

### Major gaps
- Mesh extraction still emits per-leaf tangent quads; crack-free stitching and topology deduplication are outstanding.
- CPU sampling fallbacks (`evaluateCorners`, `estimateCurvature`, and Hermite fallback) remain unimplemented, so GPU access is still mandatory.
- Exporters/UI have no hooks for the hierarchical pipeline yet; only the legacy dual contouring path is user-facing.
- No automated tests cover the hierarchical path; regression safety currently relies on manual inspection.
- Additional stats (cache hit rates, kernel timings) and balanced refinement/transition handling have not been implemented.

## Roadmap

### 1. Algorithm completion
- [ ] Implement production SDF sampling hooks for CPU fallbacks in `evaluateCorners` and `estimateCurvature` (reuse existing primitives evaluators).
- [ ] Add corner/gradient caches and plumb `enableCornerCaching` / `enableProgressiveRefinement`.
- [x] Implement GPU + host flow for zero-crossing refinement (new kernel + host batching) and populate `EdgeCrossing` refinements.
- [x] Replace the placeholder gradient math in `computeHermiteSamples` with GPU-assisted `batchGradients` and persist Hermite data per leaf.
- [x] Implement the QEF solver pipeline and store leaf vertices/normals.
- [ ] Flesh out `extractMesh` with crack-free dual mesh stitching (balanced neighbor handling, transition facets).
- [x] Wire in `HierarchicalDCProgram::detectIntersections` so CPU only needs to handle degenerate fallback cases.
- [ ] Capture additional stats (e.g., cache hit rates, SIMD query counts) to sanity-check future optimisations.

### 2. Exporter and UI integration
- [ ] Introduce a `DualContouringMethod::Hierarchical` toggle in the exporter options structs and serialize it.
- [ ] Update `DualContouringStlExporter` (and other consumers) to call into `HierarchicalOctreeBuilder` when the hierarchical option is selected.
- [ ] Add preset mapping for hierarchical quality levels in `MeshExportDialog` and surface user-facing wording/tooltips.
- [ ] Ensure progress reporting and cancellation integrate with async export infrastructure before enabling the feature.
- [ ] Keep the legacy path available as a fallback during staged rollout.

### 3. Testing and validation
- [ ] Unit tests: GPU sampling vs. reference values, curvature metrics, QEF solver correctness on analytic shapes.
- [ ] Integration tests: end-to-end mesh extraction for spheres, boxes, gyroid samples; verify watertightness and vertex counts against baselines.
- [ ] Performance regression tests measuring sampling throughput and octree sizes for representative models.
- [ ] Golden-mesh diff tooling or Hausdorff distance checks to gate CI.
- [ ] Headless stress harness that exercises cancellation, CPU fallback, and error logging.

### 4. Tooling, performance, and documentation
- [ ] Add structured logging/telemetry around construction stats and kernel timings (ties into EventLogger and Tracy).
- [ ] Document the configuration knobs and troubleshooting steps in the developer docs.
- [ ] Audit and clean up superseded plan documents (e.g., `hierarchical_dc_gpu_complete.md`) after the roadmap is in place.

## Risks & dependencies
- CPU sampling needs a reusable primitive-evaluation pipeline; until that work lands, GPU availability is a hard requirement.
- Crack avoidance relies on balanced refinement or explicit stitching; plan for extra validation time once mesh extraction is implemented.
- Exporter work touches async compute token logic—coordinate with the rendering team to avoid regressions.

## Reference files
- `src/HierarchicalDualContouring.{h,cpp}`
- `src/compute/HierarchicalDCProgram.{h,cpp}`
- `src/kernel/hierarchical_dc.cl`
- `src/compute/ProgramManager.{h,cpp}`# Hierarchical Dual Contouring (no precomputed SDF)

This plan proposes an improved dual contouring pipeline that directly queries SDF values and gradients at arbitrary positions via OpenCL kernels. It replaces the fixed-grid precomputed SDF with hierarchical distance queries and adaptive refinement, enabling better quality and efficiency on complex, highly varying geometry.

## goals
- Avoid fixed voxel grids entirely. No precomputed SDF volumes.
- Evaluate SDF and gradients on-demand for exactly the points we need.
- Build the octree breadth-first on the CPU, with large, embarrassingly parallel evaluation batches on the GPU.
- Support adaptive refinement driven by curvature/feature metrics, with multiple passes as needed.
- Keep the workflow modular: each step can fall back to CPU and is easy to test.

Non-goals (for initial delivery):
- Full GPU-side dynamic octree allocation and traversal. We’ll use CPU for structure and GPU for batched queries.
- GPU-side QEF solving (can be a later increment; CPU batched QEF using Eigen is sufficient initially).

## current state (tmp/hierarchical_dc)
Files exist but are incomplete and not integrated:
- `tmp/hierarchical_dc/HierarchicalDualContouring.h/.cpp`:
  - Defines config, node structs, and a `HierarchicalOctreeBuilder` with phases:
    - Initial octree build (BFS by level)
    - Adaptive refinement (curvature-based)
    - Zero-crossing refinement (bisection)
    - QEF vertex solving
  - Many methods are skeletons or have TODOs; several signatures are inconsistent (missing parameters), indicating WIP.
  - SDF evaluations are TODOs; intended path is through OpenCL kernels and `Primitives`.
- `tmp/hierarchical_dc/HierarchicalDCProgram.h/.cpp`:
  - Wraps an OpenCL program using sources `kernel/sdf.cl` and `kernel/hierarchical_dc.cl`.
  - Implements kernels to: evaluate corners for a level, detect intersections, estimate curvature, and batch gradients.
  - Uses existing `Primitives` buffer layout and ProgramBase infra; looks coherent.
- `tmp/hierarchical_dc/hierarchical_dc.cl`:
  - Kernels:
    - `evaluateOctreeLevel`: evaluates SDF at the 8 corners for many nodes in parallel.
    - `detectIntersections`: checks sign changes across 12 edges per node.
    - `estimateCurvature`: computes a gradient-variance proxy at leaf centers.
    - `batchGradients`: finite-difference gradients for arbitrary points.
  - Relies on `evaluateSdf(...)` from `kernel/sdf.cl` (already in the repo).

Conclusion: GPU-side pieces are in decent shape. CPU-side builder/control flow is a partially written prototype that needs finishing, compiling, and integration. Nothing is wired into the current STL exporter yet.

## architecture overview
- CPU drives octree structure and scheduling.
- GPU evaluates SDF and gradients for large batches of positions:
  - Level corner evaluations
  - Curvature metrics at leaf centers
  - Edge zero-crossing refinement (later kernel)
  - Batched gradients for Hermite/QEF
- Data representation:
  - Octree is stored as a flat array of nodes on CPU; levels are tracked for BFS.
  - Each level’s node bounds are streamed to GPU for corner evaluation.
  - Results return as contiguous arrays (8 floats per node, etc.).

## pipeline workflow
1) Initialization
- Input: root `BoundingBox`, quality preset or custom `HierarchicalConfig` (initialDepth, maxDepth, refinementIterations, thresholds).
- Create `HierarchicalOctreeBuilder(ComputeCore&, HierarchicalConfig)`.
- Prepare GPU program: `HierarchicalDCProgram` compiled with `kernel/sdf.cl` and `kernel/hierarchical_dc.cl`.

2) Build initial octree (breadth-first)
- Level 0: create root node.
- For depth d from 0..initialDepth-1:
  - Gather node bounds for level d.
  - GPU: `evaluateOctreeLevel` → 8 corner values per node.
  - CPU: `detectIntersections` via GPU kernel (or CPU fallback) to mark zero-crossing nodes.
  - CPU: nodes with sign changes are marked for subdivision if d+1 < initialDepth.
  - CPU: create next level’s children and their bounds.

3) Adaptive refinement (optional, multiple passes)
- Gather leaf nodes that intersect surface.
- GPU: `estimateCurvature` at leaf centers.
- CPU: mark leaves with curvatureMetric > threshold and depth < maxDepth.
- CPU: subdivide marked leaves; evaluate corners for new leaves (GPU) and detect intersections.
- Repeat for `refinementIterations` or until no new leaves are marked.

4) High-precision edge zero-crossing refinement
- Gather intersecting leaves; enumerate their 12 edges.
- For edges with sign changes, run iterative refinement (bisection or secant) to find zero-crossing position more precisely.
  - Kernel to add: `refineZeroCrossings` batch kernel, input pairs (p0,v0,p1,v1), output refined position and value.
  - Run for up to `maxBisectionIterations` or until `zeroCrossingTolerance`.

5) Hermite samples and QEF vertices
- For each intersecting leaf, collect refined edge crossing positions.
- GPU: `batchGradients` for all positions to get normals.
- CPU: solve QEF per leaf to obtain a robust vertex (Eigen-based batched solve is fine initially).
- CPU: stitch faces between neighboring cells (dual topology), produce triangles.

6) Output mesh
- Return vertex and index arrays to exporter.

## GPU kernels (present and planned)
Present (in `hierarchical_dc.cl`):
- evaluateOctreeLevel(min[], max[], outCornerValues[], nodeCount, primitives..., isoValue)
- detectIntersections(cornerValues[], outFlags[], nodeCount)
- estimateCurvature(leafCenters[], outCurvature[], leafCount, gradientEps, primitives...)
- batchGradients(positions[], outGradients[], count, gradientEps, primitives...)

Planned additions:
- refineZeroCrossings(edgeStart[], edgeEnd[], startVals[], endVals[], outPositions[], outVals[], count, maxIter, tol, primitives...)
  - Performs parallel bisection per edge; 8–16 iterations typically suffice.
- optional: batchSdf(positions[], outValues[], count, primitives...) for general sampling.

Notes:
- All kernels already use `evaluateSdf(...)` from `sdf.cl`, which traverses the primitive DAG (`Primitives`). That means no precomputed grid is required.
- Use structure-of-arrays for input buffers to keep memory accesses coalesced.

## traversal and scheduling strategy
- Use CPU to manage octree nodes; this keeps complexity manageable and avoids GPU-side dynamic allocation.
- For each level or pass, batch as large as possible (tens of thousands of nodes/edges) to maximize GPU occupancy.
- Memory budget: prepare buffers sized exactly for each batch; reuse allocations when possible.
- Caching:
  - Corner cache: map Morton code + corner index → value, valid across passes (optional, off by default to keep memory flat initially).
  - Gradient cache: only if profiling shows wins; gradient eval is already batched.

## integration plan
- Program Manager: add `getHierarchicalDCProgram()` and wire compilation sources.
- Builder entry point: `HierarchicalOctreeBuilder::buildOctree(BoundingBox const& bounds, Primitives const& primitives)` or pass primitives via `ComputeCore` (preferred to avoid plumbing changes).
- Exporter: add a toggle `useHierarchicalDC` in surface extraction options; when true, skip `DualContouringOctree` path and run this builder.
- Logging: re-use `EventLogger` through `ComputeCore` where available.

## testing plan
- Unit tests (GTest):
  - Corner evaluation vs CPU reference on simple SDFs (sphere, box, gyroid sample).
  - Intersection detection correctness on hand-crafted values.
  - Curvature metric sanity: higher on known high-curvature shapes.
  - Zero-crossing refinement convergence on analytic SDF edges.
  - QEF solving on synthetic Hermite data with known solutions.
- Integration tests:
  - End-to-end mesh extraction on spheres and boxes; compare vertex counts and basic metrics against baseline DC.
  - Stress test: large bounds with sparse surface; verify scalability and no hangs.
- Performance harness:
  - Measure time per phase; batch sizes; GPU time vs CPU time; collect histograms.

## milestones and tasks
- [ ] Clean up `HierarchicalDualContouring.h/.cpp` signatures and compile
  - [x] Ensure `buildOctree(...)` takes `BoundingBox` and accesses primitives from `ComputeCore`
  - [x] Fix method signatures: `buildInitialOctree(...)`, `processLevel(...)`, `evaluateCorners(...)`, `estimateCurvature(...)`, etc., to take required inputs
  - [ ] Replace all `/* TODO: implement SDF evaluation */` with calls into GPU kernels, with CPU fallback path using existing CPU SDF when available
- [ ] Implement CPU fallback SDF evaluation
  - [ ] Reuse existing CPU SDF evaluation facilities for primitives (or introduce a minimal evaluator if missing)
- [x] Wire up `HierarchicalDCProgram` into ProgramManager
  - [x] Add create/get functions, sources `kernel/sdf.cl`, `kernel/hierarchical_dc.cl`
  - [x] Ensure `Primitives` buffers are passed correctly
- [ ] Implement `refineZeroCrossings` kernel and C++ wrapper
  - [x] Kernel: parallel bisection/secant between edge endpoints, update positions/values
  - [x] C++: pack edge batches; call kernel iteratively as needed
- [ ] Implement Hermite/QEF phase
  - [x] Gather edge crossings per leaf and refine positions
  - [x] GPU `batchGradients` to compute normals
  - [x] CPU QEF solve per leaf (Eigen), return vertex positions; add simple robust clamping to cell bounds
- [ ] Mesh extraction
  - [ ] Build dual mesh from leaf vertices; handle cracks by shared vertex indices between adjacent leaves
  - [ ] Minimal manifold checks (optional for v1)
- [ ] Connect to STL export path
  - [ ] Add option flag; invoke hierarchical builder; pipe results to existing mesh write path
- [ ] Add unit/integration tests
  - [ ] Synthetic SDFs and regression baselines
  - [ ] CI hook into existing `Run Gladius Tests` preset
- [ ] Profiling and tuning
  - [ ] Batch sizes, kernel NDRange, memory reuse
  - [ ] Optional corner/gradient caches
  - [ ] Early-out heuristics (e.g., skip curvature metric if cell is clearly flat)

## design details and notes
- Zero-crossing refinement
  - Bisection per edge is robust and branch-light; each work-item handles one edge. Input arrays: startPos[3], endPos[3], startVal, endVal.
  - Optional: do 4–6 GPU iterations, then a final CPU Newton step for maximum precision.
- QEF solving
  - Use standard QEF with Hermite samples in a cell. CPU batched solve via Eigen is simple and has great numerical stability.
  - Clamp solution to cell bounds to avoid leaking vertices outside the octant.
- Curvature metric
  - Gradient-variance metric already implemented in kernel; tune `gradientEpsilon` and threshold based on tests.
- Error tolerance and limits
  - `zeroCrossingTolerance` in the 1e-5 – 1e-6 range is sufficient for STL export precision; adjust with scale of the model.

## risks and mitigations
- Kernel divergence on highly varying primitives: rely on large batches and keep kernels simple; prefer bisection.
- Memory traffic: coalesce inputs; reuse buffers per phase; pack SoA.
- Topology cracks: ensure shared edges between adjacent leaves compute the same vertex or share indices; deduplicate vertices when building topology.
- CPU fallback complexity: keep evaluators unified; if no CPU evaluator exists for some primitives, maintain a small test-only evaluator for spheres/boxes and use GPU-only for complex cases.

## try it (once implemented)
```sh
# Build
# 1) Build the project
#    Use the existing VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run a targeted test (to be added)
#    Use: "Run Gladius Tests (linux-releaseWithDebug)"
```

## references
- Ju et al., Dual Contouring of Hermite Data
- Frisken et al., Adaptively Sampled Distance Fields
- GPU bisection for root finding: common practice in ray marching / signed distance rendering

---

This plan keeps the octree data structure and scheduling on CPU for simplicity and debuggability, while pushing all heavy, independent SDF/gradient evaluations to OpenCL. It avoids the quality and memory constraints of fixed voxel grids and enables targeted, high-quality sampling exactly where needed.
