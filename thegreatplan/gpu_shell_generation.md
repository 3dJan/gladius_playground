# Plan: GPU-Based Shell Generation for Multi-Material 3D Printing

## 1. Objective
Generate "shell segments" or nested layers for multi-material 3D printing (e.g., color blending, coating, or HueForge-style layering) using the GPU. The approach must leverage the existing implicit modeling (SDF) and Dual Contouring infrastructure.

## 2. Core Concept: Implicit Shells
Instead of manipulating meshes (which is error-prone and difficult on GPU), we define shells implicitly using the Signed Distance Field (SDF).

A shell layer $i$ with thickness $T_i$ can be defined relative to the base surface ($SDF=0$).
*   **Outer Boundary**: $SDF(p) = d_{outer}$
*   **Inner Boundary**: $SDF(p) = d_{inner}$
*   **Shell Volume**: $d_{inner} < SDF(p) < d_{outer}$

For a stack of $N$ materials (e.g., White on top of Blue on top of Black):
1.  **Layer 1 (Top/Outer)**: $0 > SDF > -T_1$
2.  **Layer 2 (Middle)**: $-T_1 > SDF > -(T_1 + T_2)$
3.  **Layer 3 (Base/Core)**: $-(T_1 + T_2) > SDF$

## 3. Proposed Approaches

### Approach A: Multi-Pass Nested Surfaces (Recommended)
Generate a separate mesh for each interface boundary.
*   **Pass 1**: Extract mesh at $iso = 0$ (Outer surface).
*   **Pass 2**: Extract mesh at $iso = -T_1$ (Interface 1-2).
*   **Pass 3**: Extract mesh at $iso = -(T_1 + T_2)$ (Interface 2-3).

**Workflow:**
1.  User defines material stack and thicknesses.
2.  System computes cumulative offsets: $O_0=0, O_1=-T_1, O_2=-(T_1+T_2), \dots$
3.  For each offset $O_k$:
    *   Launch `DualContouring` (Manifold or Hierarchical).
    *   Pass $O_k$ as the `isoValue` to the GPU kernel.
    *   The GPU kernel evaluates $SDF(p) - O_k$.
    *   Extract the mesh.
4.  Export all meshes into a single 3MF file as a **multi-part object**.
5.  Assign materials to each volume in the 3MF metadata.
    *   Volume 1 (between $O_0$ and $O_1$): Defined by Mesh 0 and Mesh 1?
    *   *Slicer Behavior*: Most slicers (PrusaSlicer, BambuStudio) treat nested solids as modifiers or priority-based layers. If we export "Mesh 0" (Outer) and "Mesh 1" (Inner), and assign Material A to Mesh 0 and Material B to Mesh 1, the slicer typically renders Material A for the volume $Mesh 0 \setminus Mesh 1$ and Material B for $Mesh 1$.
    *   *Requirement*: The meshes must be strictly nested. The SDF guarantees this (assuming $T > 0$).

**Pros:**
*   **GPU Efficient**: Reuses existing highly-optimized DC kernels.
*   **Robust**: SDF offsetting is mathematically robust (no self-intersection issues like mesh extrusion).
*   **Simple Implementation**: Only requires iterating `isoValue`.

**Cons:**
*   Relies on slicer to interpret nested volumes correctly (standard workflow for multi-material).

### Approach B: Explicit Shell Volume Extraction
Generate a single watertight mesh for each shell (hollow object).
*   **SDF for Shell**: $SDF_{shell}(p) = \max(SDF(p) - d_{outer}, d_{inner} - SDF(p))$
*   This function is negative *only* inside the shell volume.
*   Running DC on this function produces a mesh with both inner and outer walls.

**Pros:**
*   Produces explicit geometry for each material.
*   Unambiguous for any software.

**Cons:**
*   **Resolution Issue**: If shell thickness is small relative to voxel size, DC may fail to resolve the thin walls, causing holes or non-manifold geometry.
*   **Performance**: Requires higher resolution grid to capture thin shells.

### Approach C: Variable Thickness (Advanced)
If thickness depends on color (e.g., for lithophanes), $T$ is not constant but a function of position $T(p)$.
*   **Kernel Modification**:
    *   Inject a "Thickness Function" into the OpenCL kernel.
    *   Evaluate $C(p) = modelColor(p)$.
    *   Compute offset $O(p)$ based on $C(p)$.
    *   Effective SDF: $SDF'(p) = SDF(p) - O(p)$.
*   **Execution**:
    *   Run DC on $SDF'(p)$.
    *   This generates a surface deformed by the color-dependent thickness.

## 4. Implementation Plan (GPU)

### 4.1 Kernel Modifications
The current `sampleCorners` kernel in `dual_contouring_sampling.cl` takes a uniform `isoValue`.
```c
values[gid] = distance - isoValue;
```

For **Variable Thickness**, we can modify it to:
```c
// Optional: Sample color to modulate thickness
float thicknessMod = 1.0f;
if (useVariableThickness) {
    float4 color = modelColor(worldPos);
    thicknessMod = calculateThicknessFromColor(color);
}
values[gid] = distance - (isoValue * thicknessMod);
```
Or pass a texture/buffer with thickness maps.

### 4.2 Host-Side Logic
1.  **`ShellGenerator` Class**:
    *   Input: `Document` (Model), `MaterialStack` (List of materials + thicknesses).
    *   Output: `std::vector<Mesh>` (One per shell interface).
2.  **Loop**:
    *   Iterate through material layers.
    *   Update `isoValue` in `ManifoldDualContouringOptions`.
    *   Call `ManifoldDualContouring::extractMesh()`.
    *   Store result.
3.  **3MF Export**:
    *   Use `MeshWriter3mf` to write multiple mesh objects.
    *   Group them under a single component/assembly.
    *   Add 3MF Material extension metadata to assign materials.

## 5. "Shell Segments" (Material Patches)
If "shell segments" implies disjoint patches (e.g., a Red patch and a Blue patch at the same depth):
*   This requires **Masking**.
*   $SDF_{Red}(p) = SDF(p)$ if $Color(p) \in Red$, else $\infty$.
*   This creates open meshes.
*   **Recommendation**: For 3D printing, continuous shells are safer. Use "Face Color Mapping" (from the other plan) to paint the *surface* faces, and use "Shells" for vertical layering.

## 6. Summary
We will proceed with **Approach A (Multi-Pass Nested Surfaces)** as the primary strategy. It is fully GPU-accelerated, robust, and fits the existing architecture. We will extend it to support **Variable Thickness** by modifying the OpenCL kernel to allow spatially varying iso-values derived from the volumetric color field.

## 7. Technical Challenges & Mitigations

### 7.1 The "Surface Projection" Problem
For variable thickness (e.g., Lithophanes), we ideally want the thickness at point $p$ to depend on the color of the **nearest surface point** $p_{surf}$.
$$ T(p) = f(Color(p_{surf})) $$
Finding $p_{surf}$ typically involves gradient descent ($p_{surf} \approx p - \nabla SDF(p) \cdot SDF(p)$).

**Risks:**
1.  **Compute Intensive**: Requires calculating gradients (multiple samples) and potentially iterating.
2.  **Instability**: In Constructive Solid Geometry (CSG), the SDF often contains discontinuities (sharp creases, unions of disparate shapes) or distortions (scaling operations). Gradient descent near these features is unstable and can map to the wrong surface point.
3.  **Distorted Fields**: If the SDF is not metric ($|\nabla SDF| \neq 1$), the distance to the surface is incorrect, making the projection inaccurate.

**Mitigation: Volumetric Color Sampling**
Instead of projecting to the surface, we rely on the fact that Gladius color definitions are **volumetric**.
*   Procedural colors (Noise, Voronoi) exist everywhere in space.
*   Image projections (Planar, Triplanar) project the color through the volume.
*   **Strategy**: We sample $Color(p)$ directly at the evaluation point.
    *   *Assumption*: The color field varies slowly or aligns with the projection axis, such that $Color(p) \approx Color(p_{surf})$.
    *   *Benefit*: Zero additional cost; stable; no gradient descent required.

### 7.2 SDF Distortion (Non-Metric Fields)
When modeling complex parts (non-uniform scaling, blending), the SDF often ceases to be a true distance field ($|\nabla SDF| \neq 1$).
*   **Consequence**: An iso-value offset of $d$ results in a physical thickness of $d / |\nabla SDF|$.
*   *Example*: If a region is scaled by 0.5, the gradient magnitude is 0.5. Requesting a 1mm shell results in a 2mm physical shell.

**Mitigation: Gradient Normalization (Optional)**
If precise thickness is critical, we can normalize the field locally in the kernel:
```c
float dist = model(p);
float3 grad = computeGradient(p); // Finite differences
float metricDist = dist / length(grad);
values[gid] = metricDist - isoValue;
```
*   **Cost**: Adds 6 extra field evaluations per sample (expensive).
*   **Decision**: Make this an optional "High Quality Shells" toggle. For most visual prints (HueForge), the distortion is acceptable or can be compensated by the user adjusting the global thickness.

### 7.3 Kernel Look-Up Table (LUT)
To support the "Multi-Pass" approach efficiently, we can pass the material configuration as a Uniform Buffer or LUT.
*   **Structure**: `float thickness_offsets[MAX_LAYERS]`
*   **Usage**: The host iterates through this array, dispatching the kernel for each layer.
*   **Optimization**: If we implement a "Single Pass, Multi-Mesh" extractor (advanced), the kernel could output a bitmask indicating which shells a voxel intersects, but standard Dual Contouring expects a single scalar field. Sticking to **one pass per layer** is safer and simpler for now.

## 8. Implementation Status

### 8.1 Host-Side Logic (Completed)
*   **Class**: `gladius::io::ShellGenerator` implemented in `src/io/3mf/ShellGenerator.h/cpp`.
*   **Functionality**:
    *   Takes `FilamentStack` and `ThicknessSolution`.
    *   Iterates through layers from top to bottom.
    *   Calculates cumulative offsets.
    *   Uses `HierarchicalOctreeBuilder` to extract meshes at `isoValue = -offset`.
    *   Returns `vector<ShellMesh>` with filament metadata.
*   **Testing**: Added unit test `tests/unittests/ShellGenerator_tests.cpp` verifying nested mesh generation on `ImplicitGyroid.3mf`.

### 8.2 Kernel Implementation (Completed)
*   **Kernel**: Added `sampleCornersVariableThickness` to `src/kernel/dual_contouring_sampling.cl`.
    *   Uses a 3D LUT (trilinear interpolation) to modulate thickness based on volumetric color.
    *   Formula: `values[gid] = distance + thickness_from_lut`.
*   **Integration**:
    *   Updated `DualContouringSamplingProgram` to expose the new kernel.
    *   Updated `GpuSamplingSession` to handle the new call.
    *   Updated `HierarchicalOctreeBuilder` to use the new kernel when `thicknessLUT` is provided in config.
    *   Updated `ShellGenerator` to build a cumulative RGB→thickness LUT via `FrontlitThicknessSolver` and pass it into `HierarchicalConfig` (per-layer, top-down).

### 8.3 Next Steps
1.  **3MF Export**: Integrate `ShellGenerator` output into `MeshWriter3mf` to write the multi-part object with material metadata.
