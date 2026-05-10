# Research: Surface-Aligned Color Sampling for Shell Generation

**Feature**: 011-surface-color-shells  
**Date**: 2026-01-09

## Problem Statement

The current shell generation samples volumetric colors at **interior evaluation points** rather than at the **surface**. For HueForge-style color-to-thickness mapping, we need the color at the visible surface (SDF=0) to drive shell thickness, not the color at arbitrary interior depths.

### Root Cause Analysis

In `sampleCornersShellVolume` kernel (dual_contouring_sampling.cl):
```c
const float4 sdfResult = model(worldPos, PASS_PAYLOAD_ARGS);
const float3 color = clamp(sdfResult.xyz, 0.0f, 1.0f);  // ← Sampled at worldPos, not surface
```

When `worldPos` is inside the model (SDF < 0), the sampled color may differ from the surface color, especially for:
- Planar/triplanar image projections
- Image mapped via UV coordinates
- Any color definition that varies with depth

## Solution Approaches Evaluated

### Approach A: Runtime Surface Projection (GPU Gradient Descent)

**Concept**: At each evaluation point, compute the surface projection using the SDF gradient, then sample color there.

```c
float3 surfacePoint = worldPos - gradient(worldPos) * distance;
float3 surfaceColor = model(surfacePoint).xyz;
```

**Pros**:
- Conceptually simple
- No preprocessing required
- Works with any color definition

**Cons**:
- **6 extra SDF evaluations per point** for gradient (central differences)
- Unstable near sharp features, CSG boundaries
- SDF may be non-metric (distorted fields) causing incorrect projections
- Performance impact: ~7x slower corner evaluation

**Assessment**: ❌ Not recommended due to performance and stability issues.

---

### Approach B: OpenVDB Sparse Grid with LUT Index Propagation

**Concept**: 
1. Sample colors at surface voxels (narrow band around SDF=0)
2. Convert colors to LUT indices and store in sparse grid
3. Propagate indices inward using morphological dilation
4. GPU kernel reads from this precomputed "color index field" instead of sampling model()

**Implementation**:
```cpp
// 1. Create Int32Grid for LUT indices (or Int8 if LUT is small)
auto lutIndexGrid = openvdb::Int32Grid::create(-1);  // -1 = unassigned

// 2. For each voxel in narrow band (|SDF| < bandwidth):
//    - If SDF close to 0: sample color, quantize to LUT index, store
//    - Mark voxel as "surface source"

// 3. Dilate active voxels inward:
openvdb::tools::dilateActiveValues(*lutIndexGrid, propagationIterations, 
                                   openvdb::tools::NN_FACE);

// 4. During shell extraction: read LUT index from grid instead of sampling color
```

**Pros**:
- Leverages OpenVDB's efficient sparse storage (only stores surface + propagated region)
- `dilateActiveValues` is highly optimized
- Memory efficient: stores indices (4 or 1 byte) not RGB (12 bytes)
- Separation of concerns: color mapping is preprocessing step

**Cons**:
- Requires additional preprocessing pass before shell extraction
- Grid resolution determines accuracy (needs to match DC resolution)
- OpenVDB not currently used in GPU kernel path (would need NanoVDB or CPU readback)
- Dilation propagates indices uniformly; doesn't follow SDF contours precisely

**Performance Estimate**:
- Grid build: O(surface_voxels) ≈ O(N²) for N³ volume
- Dilation: O(propagation_depth × active_voxels)
- Memory: ~4 bytes per active voxel (Int32) or less with Int8

**Assessment**: ⚠️ Promising but requires solving GPU access to grid data.

---

### Approach C: Mesh-Based Surface Color Sampling

**Concept**:
1. Extract outer surface mesh at SDF=0 (standard DC pass)
2. For each vertex/face, sample the volumetric color at that surface position
3. Compute thickness per vertex/face using the LUT
4. Store as per-vertex attribute or spatial data structure
5. Generate shell meshes using these precomputed surface-based thicknesses

**Implementation**:
```cpp
// Phase 1: Extract outer surface
HierarchicalOctreeBuilder builder(core, config);
builder.buildOctree(bbox);
std::vector<Eigen::Vector3f> vertices;
std::vector<uint32_t> indices;
builder.extractMesh(vertices, indices);

// Phase 2: Sample colors at each vertex
std::vector<Eigen::Vector3f> surfaceColors = FaceColorSampler::sampleVertexColors(vertices);

// Phase 3: For each vertex, compute cumulative thickness from LUT
std::vector<float> vertexThicknesses(vertices.size());
for (size_t i = 0; i < vertices.size(); ++i) {
    vertexThicknesses[i] = lookupThicknessFromLUT(surfaceColors[i], layerLUT);
}

// Phase 4: Generate offset meshes using vertex-based thickness
// For shell n: offset each vertex by -cumulative_thickness[n] along normal
```

**Challenge**: How do GPU kernels access this per-vertex data during DC?

**Sub-approaches**:

**C1: Bake into 3D texture/grid**
- Rasterize vertex thicknesses into a 3D grid
- GPU samples from this grid during DC
- Similar to Approach B but vertex-seeded

**C2: Multi-pass mesh offsetting**
- Generate outer mesh first
- Offset mesh vertices by thickness (per-vertex)
- Use offset mesh for next shell (not DC-based for inner shells)

**C3: Spatial hash map**
- Build spatial hash of vertex positions → thickness
- GPU queries nearest vertex when sampling

**Pros**:
- Uses existing FaceColorSampler infrastructure
- Accurate surface sampling (vertices are on surface by definition)
- Can leverage per-vertex normals for offset direction

**Cons**:
- Multi-pass approach (extract mesh, then generate shells)
- C2 (mesh offsetting) may cause self-intersections on high-curvature surfaces
- C1/C3 require data structure accessible from GPU

**Assessment**: ✅ Recommended with C1 variant (bake to 3D grid).

---

## Recommended Hybrid Approach

Combine the best of B and C:

### Phase 1: Surface Mesh Extraction
Extract the outer surface mesh at SDF=0 using existing HierarchicalOctreeBuilder.

### Phase 2: Vertex Color Sampling
Use FaceColorSampler to sample volumetric colors at each vertex position.
These are guaranteed to be on the surface.

### Phase 3: Build Thickness Field
Convert vertex colors to LUT indices/thicknesses and rasterize into a sparse 3D grid:
- OpenVDB Int32Grid (CPU) or
- NanoVDB grid (GPU-accessible) or
- Dense 3D texture (simpler, GPU-native)

### Phase 4: Propagate Inward
Use OpenVDB `dilateActiveValues` (or equivalent for dense grid) to fill interior voxels with nearest surface thickness.

### Phase 5: Shell Extraction
Modify GPU kernel to sample from the precomputed thickness field instead of evaluating model() color.

---

## Technology Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Grid library | OpenVDB (with potential NanoVDB for GPU) | Already in project, sparse storage, has dilation |
| Grid value type | Int16 (LUT index) | LUT resolution ≤65k, saves memory |
| Propagation method | `dilateActiveValues` with NN_FACE | Simple, follows voxel connectivity |
| GPU access | Convert to dense texture for OpenCL | Simpler than NanoVDB integration |
| Alternative for GPU | Dense 3D buffer | Bounded memory, direct OpenCL access |

---

## Open Questions

1. **NanoVDB integration**: Is it worth adding NanoVDB for direct GPU sparse grid access?
   - Pros: Memory efficient for large volumes
   - Cons: Additional dependency complexity, NanoVDB learning curve
   - **Decision**: Start with dense 3D texture/buffer for simplicity; optimize later if needed

2. **Grid resolution**: Should thickness grid match DC resolution exactly?
   - Yes for accuracy, but can use lower resolution with trilinear interpolation
   - **Decision**: Use DC maxDepth-derived resolution with interpolation

3. **Edge cases**: What if a surface vertex projects to an unmapped color?
   - Use default LUT index (e.g., thickest layer configuration)
   - Log warning for debugging

---

## References

- [ShellGenerator.cpp](../../gladius/src/io/3mf/ShellGenerator.cpp) - Current implementation
- [dual_contouring_sampling.cl](../../gladius/src/kernel/dual_contouring_sampling.cl) - GPU kernels
- [FaceColorSampler.h](../../gladius/src/io/3mf/FaceColorSampler.h) - Existing color sampling
- [OpenVDB Morphology.h](vcpkg_installed/.../openvdb/tools/Morphology.h) - Dilation API
