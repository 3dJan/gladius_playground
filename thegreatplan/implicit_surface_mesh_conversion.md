# Implicit Surface Mesh Conversion Study

## Objective
Understand and document Gladius's end-to-end pipeline for converting implicit surface representations into polygonal meshes, specifically in the context of STL and 3MF (core) exports.

## Investigation Steps
1. **Review existing documentation**
   - Check `docs/architecture` and any relevant design notes for references to implicit surfaces or meshing workflows.
   - Purpose: quickly gather high-level context and terminology before diving into source code.

2. **Map implicit surface representation modules**
   - Search the codebase (likely under `library/` and `src/`) for classes and functions related to implicit geometry (keywords: `implicit`, `signedDistance`, `isosurface`, `levelset`).
   - Catalogue primary data structures (e.g., signed distance fields, procedural definitions), noting where they are defined and how they are instantiated.

3. **Identify meshing/conversion algorithms**
   - Locate implementations responsible for implicit-to-mesh transformations (e.g., marching cubes, dual contouring).
   - Record the entry points, algorithm variants, and key parameters influencing mesh resolution or quality.

4. **Trace export workflow integration**
   - Inspect STL (`CliWriter`, `StlWriter`) and 3MF export modules to determine how they request mesh generation from implicit sources.
   - Follow the call graph from export triggers through any intermediate abstractions (e.g., scene graph, resource managers) down to meshing routines.

5. **Understand acceleration and hardware utilization**
   - Investigate whether GPU/OpenCL components participate in implicit meshing, focusing on files like `ComputeContext`, `ImageStackOCL`, or other GPGPU utilities.
   - Document any runtime switches or configuration hooks that affect the pipeline.

6. **Compile a comprehensive summary**
   - Synthesize gathered insights into a coherent narrative covering data flow, key classes/functions, algorithms, and configuration points.
   - Highlight edge cases, performance considerations, and outstanding questions for future deep-dives.

## Tools & Resources
- Workspace search (keyword/regex) to quickly locate relevant symbols.
- `read_file` for detailed inspection of identified modules.
- Existing architectural documentation within `docs/`.

## Deliverables
- A written report summarizing the implicit-to-mesh conversion pipeline, suitable for onboarding new contributors or informing optimization work.
- Pointers to critical source files and configuration options affecting mesh export quality/performance.

## Strategic Options for Improved Iso-Surface Meshing

### Requirements Recap
- Preserve high-fidelity implicit surfaces used for 3D printing (watertight, manifold, sharp-feature aware meshes).
- Support adaptive resolution to minimize triangle count without losing detail.
- Integrate with existing OpenCL distance evaluation kernels; avoid large CPU-only bottlenecks.
- Stay within the C++ toolchain (CMake, vcpkg); prefer libraries with liberal licenses and long-term maintenance.

### Candidate Approaches
1. **Tune Existing OpenVDB Pipeline**
   - Investigate `openvdb::tools::QuadraticMarchingCubes` / dual contouring variants (if available in the current VDB version) and experiment with tighter `VolumeToMesh` adaptivity thresholds.
   - Post-process with OpenVDB’s `tools::meshToVolume` / `tools::clip` utilities plus Taubin smoothing to reduce noise while retaining edges.
   - Pros: Minimal integration work, reuses current grid generation. Cons: Still marching-cubes-derived; sharp features remain challenging.

2. **Octree-Based Dual Contouring (DC / Adaptive DC)**
   - Build an octree over the SDF using hierarchical sampling (OpenCL kernels can evaluate corner SDF/gradients in batches).
   - For each active cell, solve a quadratic error function (QEF) to place vertices, preserving sharp edges when Hermite data (normals) are available.
   - Recommended references: Ju et al., *Dual Contouring of Hermite Data*; Ancel et al., *Adaptive Dual Contouring*.
   - Integration options:
     - Implement in-house leveraging existing OpenCL distance queries and Eigen for QEF solves.
     - Evaluate open-source DC implementations such as `DCGrid` (GPL), `mc-dc` (MIT, header-only) for adaptation.
   - Pros: Excellent feature preservation, adaptive triangle density. Cons: Requires new infrastructure (octree builder, crack prevention between LOD levels).

3. **Hermite RBF / Variational Implicit Fitting with Remeshing**
   - Sample surface points + normals via OpenCL, then run a variational re-meshing (e.g., `remesh_surf` in libigl, or CGAL’s isotropic remeshing) to produce uniform triangles.
   - Suitable as a post-process after DC or refined marching cubes to improve element quality.
   - Pros: High-quality triangles, controllable edge length. Cons: Additional passes and potential shrinkage unless volume constraints enforced.

4. **CGAL Surface Mesh Generation (SMG)**
   - Utilize `CGAL::make_surface_mesh` with implicit surface functors that call into our OpenCL distance kernels.
   - Provides adaptive refinement (criteria on facet size, approximation error, and curvature) and guarantees manifold, watertight meshes.
   - Available through vcpkg; permissive LGPL/GPL mix (check component licensing).
   - Pros: Battle-tested for sharp features and adaptive meshing. Cons: CPU-centric; may need parallelization or caching to meet performance targets.

5. **Open3D / PyMesh / Manifold Libraries**
   - Use surface reconstruction pipelines (Poisson, Screened Poisson) on dense point clouds sampled from the SDF.
   - Pros: Produces smooth meshes with controllable detail. Cons: Indirect workflow, may oversmooth sharp edges; additional dependency footprint.

6. **GPU-Accelerated Surface Nets / Marching Tetrahedra**
   - Implement Surface Nets (combined MC/DC hybrid) using OpenCL to exploit existing GPU infrastructure, enabling adaptive quad topology and conservative simplification.
   - Reference implementation: `OpenCL SurfaceNets` (MIT) demonstrates good balance between speed and mesh quality.
   - Pros: Fits compute pipeline, can output quads for subsequent subdivision. Cons: Requires careful crack fixing on LOD boundaries.

### Library & Dependency Considerations
- **CGAL (Surface Mesh Generation, Triangulation packages)**: Available in vcpkg (`cgal`), requires Boost + GMP/MPFR. Ensure license alignment (LGPL for SMG, GPL for some features).
- **libigl**: Header-only, MPL2.0. Provides marching cubes variants, quadric error metric simplification, isotropic/anisotropic remeshing.
- **Open3D**: Apache-2.0, heavy dependency graph (Eigen, FLANN). Offers Poisson reconstruction and mesh decimation.
- **MeshOptimizer / Fast-Quadric-Mesh-Simplifier**: Good for post-export simplification (triangle reduction with error bounds); MIT-licensed, lightweight.
- **Instant Meshes / Manifold**: Useful for quad-dominant output, but introduces new pipelines (less direct for STL needs).

### Integration Path Sketch
1. **Short Term Experiments**
   - Benchmark OpenVDB parameter tuning (`adaptivity`, `isovalue`) and smoothing filters.
   - Prototype CGAL SMG integration using CPU distance callbacks (wrap OpenCL kernel via command queue or fallback to CPU SDF for comparison).

2. **Mid Term Prototype**
   - Build an octree-based dual contouring prototype (CPU control, GPU-accelerated sampling). Validate edge preservation on representative 3D printing models.
   - Compare mesh quality metrics (triangle count, minimum dihedral angle, Hausdorff distance) against OpenVDB output.

3. **Long Term Adoption**
   - Select the pipeline that balances quality and performance. Harden integration (threading, caching, cancellation hooks) and expose quality presets to the UI/export dialogs.
   - Add automated regression tests measuring geometric deviation and manifoldness for exported meshes.

4. **Post-Processing Enhancements**
   - Integrate optional isotropic remeshing (CGAL/libigl) with edge-length targets tied to print resolution.
   - Offer mesh repair (e.g., via `igl::copyleft::cgal::remesh_self_intersections` or Netfabb-compatible output) as final cleanup before STL/3MF serialization.

## Draft Plan: Octree-Based Dual Contouring Implementation

### Objectives & Success Criteria
- Generate watertight, manifold meshes that closely approximate the implicit surface (Hausdorff error ≤ target tolerance, default 0.02 mm).
- Preserve sharp features by leveraging Hermite data (surface normals) in QEF minimization.
- Achieve adaptive triangle density: high refinement near detail/curvature, coarse elsewhere, with user-tunable edge-length targets.
- Keep throughput acceptable: target ≤ 2× the runtime of current OpenVDB pipeline for representative models (e.g., 200 mm³ build volume at 0.1 mm base resolution).
- Integrate seamlessly with existing export flow (STL, 3MF) and respond to cancellation/async updates.

### High-Level Pipeline
1. **Bounding Volume Setup**
   - Reuse `ComputeCore::updateBBox()` to obtain the current model bounding box, expand by configurable safety margin (default 5–10 mm).

2. **Adaptive Octree Construction**
   - Start from root cell covering the padded bounding box.
   - Recursively subdivide cells based on:
     - Sign change detection across cell corners (via SDF sampling).
     - Estimated surface curvature/feature score derived from corner gradients.
     - User-specified maximum depth (controls finest voxel size).
   - Maintain a pool of leaf cells tagged as **intersecting** (surface passes through) or **empty/solid**.

3. **Hermite Data Gathering**
   - For each intersecting leaf cell edge where the SDF changes sign, compute:
     - Precise zero-crossing location via linear interpolation or 1D root finder (Newton).
     - Surface normal at the crossing using central-difference gradient (requires 6 additional SDF samples around the point).
   - Pack Hermite samples into per-leaf structures.

4. **Vertex Placement (QEF Solver)**
   - Solve the quadratic error function per leaf: minimize \(\sum_i (n_i \cdot (x - p_i))^2\) subject to constraints (e.g., stay within cell bounds).
   - Apply Clarkson’s method or singular value decomposition (Eigen) for robustness.
   - Clamp solutions to the cell to avoid vertices drifting outside (important near thin features).

5. **Topology Extraction**
   - Build faces by connecting dual vertices of adjacent leaf cells that share a face and differ in sign classification.
   - Handle multi-resolution adjacency by inserting **stitching quads/triangles** to prevent T-junction cracks (standard Adaptive Dual Contouring technique).

6. **Mesh Output & Post Processing**
   - Assemble triangle mesh (or quad mesh first, then triangulate with consistent winding).
   - Optionally run lightweight smoothing or edge collapse passes constrained by error tolerance.
   - Compute per-vertex normals using Hermite data or by evaluating the implicit gradient at vertex positions for shading.

### Data Structures
- `OctreeNode`
  - Bounds (center, half-extent).
  - Children indices (8) or leaf flag.
  - Corner SDF values and optional gradients.
  - Error metrics (curvature, feature score) to guide subdivision.
- `LeafCellData`
  - Hermite samples: list of `(position, normal)` pairs, one per sign-changing edge.
  - Precomputed QEF matrices/vectors for reuse.
  - Final dual vertex position.
- GPU staging buffers
  - Corner SDF sample results batched per node level.
  - Gradient samples for Hermite computation.

### OpenCL Integration Strategy
- **Corner Sampling Kernel**
  - Input: array of cell corners (positions), constant buffers for implicit evaluation.
  - Output: SDF values written to buffer; optionally, coarse gradients via finite differences.
- **Hermite Sampling Kernel**
  - Input: candidate edge sample positions; compute high-accuracy distance using existing SDF kernel.
  - Output: zero-crossing SDF, gradient vector.
- Use command queues already managed by `ComputeCore`; honor cancellation tokens in async pipeline.
- Employ double-buffering to overlap CPU octree processing with GPU sampling.

### Adaptive Subdivision Criteria
- **Sign Change**: if corner signs differ, subdivide until max depth or error threshold reached.
- **Curvature Estimate**: use gradient variation across corners; subdivide when variation exceeds user threshold.
- **Feature Flags**: near subtractive operations or boolean boundaries (detect via node metadata) allow forcing higher resolution.
- **Volume Error Bound**: estimate via Lipschitz constant to guarantee Hausdorff error below target.

### Crack Prevention & Stitching
- Implement balanced subdivision: ensure neighboring cells differ in depth by at most one level (force refinement on coarser neighbors).
- For depth transitions, emit face-splitting quads and triangulate with consistent orientation.
- Provide post-assembly validation to check for non-manifold edges/vertices.

### Phased Implementation Plan
1. **Prototype (Sprint 1–2)**
   - CPU-only octree + QEF (no GPU acceleration) using existing SDF evaluation for small models.
   - Validate mesh quality vs. OpenVDB on benchmark parts; measure Hausdorff distances (libigl `point_mesh_squared_distance`).

2. **GPU Sampling Integration (Sprint 3–4)**
   - Introduce OpenCL kernels for bulk SDF sampling; integrate with `ComputeCore` resource management.
   - Add caching of sample results to avoid redundant evaluations within octree refinement.

3. **Adaptive Refinement + Stitching (Sprint 5)**
   - Implement balanced refinement and T-junction stitching.
   - Add curvature-based refinement heuristics; expose quality presets.

4. **Exporter Integration (Sprint 6)**
   - Extend `MeshExporter` / `MeshExporter3mf` to select DC pipeline.
   - Provide fallbacks (e.g., revert to OpenVDB if DC fails) and progress reporting hooks.

5. **Optimization & QA (Sprint 7–8)**
   - Profile memory and runtime; optimize data structures (e.g., pool allocators, SoA corner caches).
   - Add automated regression tests comparing deviation metrics and ensuring manifoldness (via CGAL mesh validity checks).

### Testing & Validation
- **Unit tests** for QEF solver stability (known Hermite configurations with analytic solutions).
- **Property tests** verifying manifold output on randomized implicit primitives (spheres, gyroids, CSG combos).
- **Performance benchmarks** vs. OpenVDB on representative datasets (record runtime, memory, triangle count, error metrics).
- **Integration tests** in export pipeline: ensure STL/3MF exports succeed, bounding boxes match expected tolerance, async cancellation works.

### Risks & Mitigations
- **Octree memory blow-up**: enforce max depth, adopt sparse representations (hash-grid or morton-coded nodes).
- **Crack artifacts**: invest early in balanced refinement, include debug visualizations of cell adjacency.
- **Gradient noise**: smooth SDF sampling (averaging) or apply normal regularization before QEF solving.
- **Performance regressions**: keep OpenVDB path as fallback; add telemetry for sampling counts and queue wait times.

### Tooling & Dependencies
- Linear algebra: Eigen (already in tree) for SVD/QEF.
- Optional: nanoflann or custom KD-tree if we add nearest-neighbor queries for validation.
- Debug visualization: write out octree slices or dual vertices via `.ply` for inspection.

### Documentation & User Experience
- Add UI preset options (Fine, Balanced, Draft) mapping to max depth / error thresholds.
- Document quality trade-offs and recommended settings per printer resolution.
- Update exporter progress dialog to reflect DC stages (sampling, solving, meshing).

## Implementation Work Breakdown Structure (WBS)

### Phase 0 – Project Setup & Baselines
- **Task 0.1**: Capture baseline metrics of current OpenVDB export on three reference models (simple mechanical part, organic lattice, large build) – runtime, triangle count, Hausdorff error vs analytic ground truth or dense sampling.
- **Task 0.2**: Prepare a unit-test fixture dataset (implicit primitives + expected properties) to reuse through future phases.
- **Task 0.3**: Establish a feature branch strategy (`feature/dual-contouring`) and CI pipeline steps for new unit/integration tests.

#### Phase 0 Progress (2025-10-29)
- Added `MeshBaseline_tests.cpp` (gtest) exercising the current OpenVDB preview mesh path on three reference assemblies: `SimpleGyroid.3mf`, `ImplicitGyroid.3mf`, and `SphereInACage_small.3mf`.
- Recorded baseline outputs captured from the RelWithDebInfo build (ReleaseWithDebug preset):
   - **SimpleGyroid** — 412,548 triangles, mesh bounds $\bigl[\mathbf{m}_{\min} = (0.00363, 0.00354, 0.00345)\ \text{mm},\ \mathbf{m}_{\max} = (9.91768, 9.91761, 9.91766)\ \text{mm}\bigr]$, runtime ceiling set to 200 ms.
   - **ImplicitGyroid** — 846,476 triangles, mesh bounds $\mathbf{m}_{\min} = (-7.63934, -1.95782, -0.000014)\ \text{mm}$, $\mathbf{m}_{\max} = (64.15601, 73.53230, 49.60936)\ \text{mm}$, runtime ceiling set to 350 ms.
   - **SphereInACage (small)** — 62,682 triangles, mesh bounds $\mathbf{m}_{\min} = (79.72140, 91.07452, 45.00000)\ \text{mm}$, $\mathbf{m}_{\max} = (89.64327, 100.99639, 54.92188)\ \text{mm}$, runtime ceiling set to 120 ms.
- Bounding-box assertions ensure generated meshes remain within the ComputeCore safety padding, while stricter mesh baselines guard against regressions.
- These fixtures satisfy Task 0.2 and provide concrete metrics toward Task 0.1; Hausdorff-error sampling remains open and will be tackled alongside CPU/DC prototypes.

### Phase 1 – CPU Prototype (Single-Threaded)
- **Task 1.1**: Implement minimal `OctreeNode` and builder operating on CPU with synchronous SDF sampling via existing `ComputeCore::precomputeSdfForBBox` (reuse buffer, no OpenCL yet).
- **Task 1.2**: Implement QEF solver (Eigen-based) with regression tests using synthetic Hermite data (planes, corners).
- **Task 1.3**: Generate meshes for small test cases (unit cube, sphere, cylinder blend) and compare with reference meshes (visual + geometric checks).
- **Task 1.4**: Add command-line prototype driver (`tools/dc_prototype`) for rapid iteration and debugging.

#### Phase 1 Progress (2025-10-29)
- Introduced `QuadraticErrorFunction`, a CPU-side Hermite QEF accumulator that normalizes input normals, performs SVD-based least-squares solves, and supports clamping the solution to octree cell bounds.
- Added unit coverage in `DualContouringQef_tests.cpp` for plane-intersection accuracy, bound clamping, and error handling when insufficient Hermite samples are supplied; tests execute without relying on OpenCL so they harden Task 1.2 foundations early.
- With Task 1.1 and Task 1.2 in place, the next milestone is Task 1.3: wiring Hermite edge sampling from octree leaves and emitting provisional meshes for analytical primitives.
- Added `DualContouringOctree_Test.LeafNodesProduceHermiteSamplesAndVertices` to assert that intersecting leaves now record Hermite samples and emit vertices, marking the first end-to-end validation for Task 1.3.

### Phase 2 – GPU Sampling Integration
- **Task 2.1**: Design OpenCL kernels for batched corner sampling and Hermite gradient evaluation; integrate with `ComputeCore` queue management.
- **Task 2.2**: Implement host-side cache for sampled SDF values keyed by Morton index to reduce redundant evaluations.
- **Task 2.3**: Validate consistency between CPU-only and GPU-assisted sampling (unit tests comparing outputs within tolerance).
- **Task 2.4**: Profile sampling throughput; optimize kernel launch configuration, memory transfers, and buffer reuse.

#### Phase 2 Execution Plan – GPU Sampling Integration

**Objectives**
- Eliminate the CPU sampling bottleneck by evaluating octree corner and Hermite samples on the GPU while keeping the CPU responsible for octree topology decisions.
- Maintain numerical parity with the existing CPU sampler (absolute error ≤ 1e-5 for SDF values, ≤ 5e-4 for gradients) and guarantee deterministic results independent of queue execution order.
- Reuse existing `ComputeCore` device discovery, context lifetime, and cancellation plumbing to avoid bespoke OpenCL management code.

**Kernel Suite**
1. `CornerSampler.cl` — accepts a structured buffer of Morton-sorted octree corners (position + level metadata) and writes SDF values to a coalesced output buffer. Uses vectorized float4 loads where possible, with local memory staging for repeated node evaluations.
2. `HermiteSampler.cl` — takes edge midpoint candidates plus precomputed corner SDFs; refines zero-crossing via secant iteration (max 4 steps) and emits `(position, gradient)` tuples using central differences. Shares gradient code with existing FunctionGradient kernels to stay numerically consistent.
3. Optional `GradientJacobian.cl` (stretch goal) — computes curvature proxy (∥∂n/∂x∥) to feed adaptive refinement heuristics in Phase 3; not on critical path but kernel layout will be prototyped now to avoid later rewrites.

**Host-Orchestrated Pipeline**
- Introduce `GpuSamplingSession` (RAII wrapper) inside `DualContouringOctree` that batches leaf expansion requests into fixed-size workgroups (default 8k corners / 16k hermite edges) before dispatching to `ComputeCore::enqueueKernel`.
- Maintain double-buffered staging areas so that while the GPU processes batch *N*, the CPU consumes results from batch *N-1* to update octree nodes. Synchronize via `cl::Event` fences, honoring cancellation checks between batches.
- Extend `ComputeContext` with lightweight buffer pool APIs (e.g., `acquireFloatBuffer(bytes)` / `releaseBuffer(id)`) to avoid per-dispatch allocations and to allow future reuse by other meshing stages.

**Caching & Data Layout**
- Adopt 64-bit Morton indices for octree corners; store them alongside sampled values in a robin-hood hash map (`CornerSampleCache`). The cache is warmed by the GPU output buffer and queried before enqueuing new work to minimize redundant sampling when neighboring leaves trigger overlapping requests.
- For Hermite samples, reuse cached corner SDFs to seed the secant search, only invoking GPU refinement when interpolation residual > tolerance. Persist results in `HermiteSampleCache` keyed by (cell id, edge id) to support later QEF recomputation without re-sampling.

**Validation & Instrumentation**
- Add `DualContouringSampling_tests.cpp` comparing GPU vs CPU sampling on deterministic fixture geometries (sphere, gyroid patch, boolean union). Assertions cover SDF/gradient tolerance, zero-crossing location, and edge-case handling when gradients approach zero.
- Integrate sampling counters into `EventLogger`: `gpu_corner_batches`, `gpu_hermite_batches`, `cache_hits`, `cache_misses`, average kernel duration. Expose summary in prototype CLI and exporter logs for regression tracking.
- Create a debug mode that dumps sampled point clouds (`.ply`) to aid visual inspection when discrepancies arise.

**Performance Targets & Profiling Plan**
- Aim for ≥ 5× speedup over CPU sampling on reference models (SimpleGyroid, ImplicitGyroid) measured as corners evaluated per second. Maintain GPU utilization ≥ 70% per Nsight Systems trace.
- Use `ComputeCore`’s existing profiling hooks to measure kernel enqueue latency, buffer transfer cost, and occupancy. Schedule weekly profiling sessions to track progress and adjust batch sizes / workgroup dimensions.

**Milestones**
1. *Week 1*: Kernel prototypes running against synthetic data, integrated build rules, unit tests validating numerical parity.
2. *Week 2*: Corner sampling wired into octree builder with cache hits recorded; CPU fallback path maintained behind runtime flag.
3. *Week 3*: Hermite sampler integration, dual-buffer orchestration stable, instrumentation emitting metrics.
4. *Week 4*: End-to-end GPU sampling default-enabled for prototype CLI; performance regression report generated vs baseline; outstanding tuning items triaged.

**Risks & Mitigations**
- *GPU starvation due to small batches*: implement adaptive batch sizing that grows when device reports low occupancy, shrinks when memory pressure detected.
- *Driver variability across vendors*: add capability detection (e.g., max workgroup size, local memory limit) and ship fallback CPU path when kernels fail to build; document tested hardware matrix.
- *Numerical drift from GPU math*: enforce use of fused multiply-add where available, add comparative unit tests using random seeds to catch regressions, and allow developer flag to force CPU verification runs.

#### Phase 3 Progress (2025-11-04)
- Implemented **curvature-based adaptive refinement**:
  - Added gradient variance calculation across octree corners
  - Configurable `curvatureThreshold` parameter (default 0.5)
  - Enhanced `shouldSubdivide()` to detect high-curvature regions
  - Enables better feature detection for complex geometries

- Implemented **balanced octree refinement**:
  - Multi-pass balancing algorithm enforces depth difference ≤ 1 between neighbors
  - `enforceBalance()` method performs iterative subdivision until constraint satisfied
  - Added `balancePassSubdivisions` metric to track refinement iterations
  - Significantly reduces T-junction artifacts without explicit stitching
  - New test `BalancedRefinementReducesCracks` validates the implementation

- Added **quality preset system**:
  - `DualContouringQuality` enum: Draft/Balanced/Fine/UltraFine/Custom
  - `applyPreset()` automatically configures resolution, depth, and refinement parameters
  - Presets balance quality vs performance for different use cases
  - Integrated with export options for easy user selection

- **Test Coverage**: All 8 dual contouring tests passing, including new balanced refinement test

**T-Junction Handling Strategy**:
The balanced refinement implementation provides the foundation for crack-free meshes by ensuring neighboring cells differ by at most one octree level. This approach is more robust than explicit T-junction stitching because:
1. It prevents problematic multi-level transitions at the octree construction stage
2. The existing quad emission logic in `generateFaces()` naturally handles single-level transitions
3. Face generation already skips cells without vertices, preventing gaps

**Next Steps for T-Junction Stitching** (if needed):
- Add explicit transition quad detection at depth boundaries
- Implement Marching Cubes-style edge case tables for hanging nodes
- Consider alternative: keep balanced refinement as primary strategy

### Phase 4 – Integration into Export Pipeline
- **Task 3.1**: Implement balanced refinement to enforce depth difference ≤ 1 between neighboring cells.
- **Task 3.2**: Add curvature/error heuristics and expose parameters via configuration struct (tie to exporter presets).
- **Task 3.3**: Implement transition stitching (quad splitting) and add tests ensuring no T-junctions remain (mesh audit using libigl or custom checks).
- **Task 3.4**: Extend prototype driver to export intermediate octree diagnostics (e.g., `.ply` of cell centers) for analysis.

### Phase 4 – Integration into Export Pipeline
- **Task 4.1**: Create `DualContouringMesher` service class with API `generateMesh(ComputeCore&, MeshingParameters)` returning `gladius::Mesh`.
- **Task 4.2**: Modify `MeshExporter`/`MeshExporter3mf` to optionally use DC pipeline; add configuration UI binding (Quality dropdown).
- **Task 4.3**: Introduce progress reporting hooks (sampling %, solving %, stitching %) and wire to existing export dialogs.
- **Task 4.4**: Add fallback logic and telemetry (log success/failure, runtime, key metrics) for monitoring.

### Phase 5 – Quality Assurance & Optimization
- **Task 5.1**: Implement automated Hausdorff distance comparisons using dense point sampling (libigl or CGAL) within CI.
- **Task 5.2**: Add mesh validity checks (manifoldness, watertightness) using CGAL or custom validators.
- **Task 5.3**: Benchmark against baseline on expanded dataset; tune defaults to meet performance target.
- **Task 5.4**: Perform memory profiling; introduce pooling or streaming if needed to reduce peak usage.

### Phase 6 – Documentation & Release Readiness
- **Task 6.1**: Document new exporter options and recommended presets in user manual.
- **Task 6.2**: Create developer documentation covering octree data structures, kernels, and troubleshooting steps.
- **Task 6.3**: Prepare release notes summarizing improvements and known limitations.
- **Task 6.4**: Schedule cross-team review (rendering + UI + QA) before enabling feature by default.

## Open Questions & Next Steps
- Confirm licensing acceptability for potential helper libraries (e.g., libigl for error metrics).
- Decide tolerance thresholds (default Hausdorff, curvature) based on print resolution requirements.
- Determine whether to support quad output for downstream processing or triangulate immediately.
- Establish telemetry strategy to collect real-world usage metrics post-release.
