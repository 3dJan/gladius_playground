# Feature Specification: Fast Mesh Simplification for Export

**Feature Branch**: `026-fast-mesh-simplification`  
**Created**: 2026-03-26  
**Status**: Draft  
**Input**: User description: "As a user I want to be able to apply a fast and effective mesh simplification when exporting to mesh based formats. The mesh simplification should work for all mesh extraction methods. The resulting meshes need to be printable (closed, manifold etc.). Colors should be preserved (resampled/assigned after simplification)."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Fast Simplification During STL/3MF Export (Priority: P1)

A user has designed an implicit model and wants to export it as an STL or 3MF file for 3D printing. The extracted mesh has a high triangle count (e.g., 500K–2M triangles). The user enables mesh simplification in the export dialog and selects a target triangle count or reduction percentage. The simplification runs quickly (seconds, not minutes) and produces a mesh that is significantly smaller while preserving the shape accurately. The user can then import the result into a slicer (PrusaSlicer, OrcaSlicer, Bambu Studio, etc.) and print it without errors.

**Why this priority**: This is the core value proposition — users need fast, effective simplification to make exported meshes practical for slicing and printing. Without this, exports are either too large for slicers to handle efficiently or take too long to simplify.

**Independent Test**: Export a model with ~1M triangles using each mesh extraction method (Layered Marching Cubes, Dual Contouring, Manifold Dual Contouring), enable simplification to 50% reduction. Verify the result is produced within a reasonable time, has the target triangle count, and passes mesh validity checks.

**Acceptance Scenarios**:

1. **Given** a model with 1M triangles extracted via any mesh extraction method, **When** the user enables simplification with a 50% target reduction and exports, **Then** the exported mesh has approximately 500K triangles and the export completes within 10 seconds on a typical desktop.
2. **Given** a model exported with simplification enabled, **When** the exported file is loaded in a slicer, **Then** the slicer reports no mesh errors (non-manifold edges, holes, inverted normals).
3. **Given** a model with fine surface details, **When** simplification is applied with default settings, **Then** the simplified mesh visually matches the original with no obvious loss of important features (smooth surfaces stay smooth, sharp edges stay sharp).

---

### User Story 2 - Printable Output Guaranteed (Priority: P1)

A user exports a simplified mesh and needs it to be directly printable. The simplification must never introduce mesh defects: no flipped triangles, no non-manifold edges, no holes, and no self-intersections. The mesh must remain closed (watertight) after simplification.

**Why this priority**: Printability is a hard requirement — a mesh that looks good but cannot be printed defeats the purpose of export. This is equally important as speed.

**Independent Test**: Export 10 different models with simplification at various reduction levels (25%, 50%, 75%). Run each result through a mesh validation tool. All must pass watertight and manifold checks.

**Acceptance Scenarios**:

1. **Given** a watertight manifold mesh from any extraction method, **When** simplification is applied at any reduction level up to 75%, **Then** the result remains watertight and manifold.
2. **Given** a mesh being simplified, **When** an edge collapse would create a non-manifold edge or flip a triangle, **Then** that collapse is rejected and the algorithm continues with the next best candidate.
3. **Given** a mesh with boundary edges (non-closed input), **When** simplification is applied, **Then** boundary edges are preserved (boundary vertices are not collapsed or are only collapsed along the boundary).

---

### User Story 3 - Color Preservation After Simplification (Priority: P2)

A user has a model with volumetric color (e.g., per-vertex or per-face color from an implicit color function). After simplification removes triangles and moves vertices, the colors on the simplified mesh must still accurately represent the original design intent. Colors are resampled or reassigned from the original color source after simplification, so that the simplified mesh has correct colors at its new vertex/face positions.

**Why this priority**: Color support is important for multi-material and decorative prints, but the basic geometric simplification must work correctly first. Color resampling is an additional step that builds on top of working simplification.

**Independent Test**: Export a colored model with simplification enabled. Compare the per-vertex colors of the simplified mesh against the expected colors at those positions (sampled from the original color function). Verify color deviation is within acceptable tolerance.

**Acceptance Scenarios**:

1. **Given** a model with per-vertex colors exported with simplification enabled, **When** the export completes, **Then** the colors on the simplified mesh are resampled from the original color source at the new vertex positions.
2. **Given** a simplified colored mesh, **When** imported into a slicer that supports color 3MF, **Then** the color distribution visually matches the original unsimplified version.
3. **Given** a model without colors, **When** simplification is applied, **Then** the simplification works identically (color resampling is a no-op).

---

### User Story 4 - User Controls for Simplification (Priority: P2)

A user wants control over the simplification process. In the export dialog, the user can choose between simplification modes: target triangle count, target reduction percentage, or error-bounded (simplify until a quality threshold is reached). The user can also choose between speed-optimized simplification (pure geometric) and quality-optimized simplification (SDF-aware, which evaluates the implicit function to measure surface deviation).

**Why this priority**: Different use cases need different trade-offs. A quick draft export needs speed; a final production export needs precision. Giving users this choice builds on the working simplification from P1.

**Independent Test**: Open the export dialog and verify all simplification options are available and functional. Export with each mode and verify the output meets the specified target.

**Acceptance Scenarios**:

1. **Given** the export dialog with simplification enabled, **When** the user selects "target triangle count" and enters 100,000, **Then** the exported mesh has approximately 100,000 triangles.
2. **Given** the export dialog, **When** the user selects "target reduction" and enters 60%, **Then** the exported mesh has approximately 40% of the original triangle count.
3. **Given** the export dialog, **When** the user selects "error-bounded" mode, **Then** the system simplifies as aggressively as possible while staying within the configured error tolerance.
4. **Given** the export dialog, **When** the user switches between "fast (geometric)" and "quality (SDF-aware)" simplification, **Then** the corresponding algorithm is used and the result reflects the trade-off (fast mode is faster, quality mode preserves surface detail better).

---

### User Story 5 - Progress Feedback During Simplification (Priority: P3)

When simplification takes more than a brief moment (e.g., on very large meshes), the user sees a progress indicator showing how the simplification is progressing. The user can cancel the operation if it takes too long.

**Why this priority**: Good UX, but not essential for the feature to deliver value. The fast algorithm should make this less critical than with the current slow implementation.

**Independent Test**: Export a very large model (2M+ triangles) with simplification and verify a progress indicator appears and updates. Cancel the operation mid-way and verify the application returns to a clean state.

**Acceptance Scenarios**:

1. **Given** a mesh with 2M+ triangles being simplified, **When** simplification takes more than 1 second, **Then** a progress indicator is displayed showing approximate completion percentage.
2. **Given** an ongoing simplification, **When** the user clicks cancel, **Then** the operation stops within 1 second and no corrupted files are written.

---

### Edge Cases

- What happens when the target triangle count is higher than the current count? Simplification is skipped and the original mesh is exported unchanged.
- What happens when the mesh is already at minimal triangle count (e.g., a single tetrahedron)? Simplification returns the mesh unchanged.
- What happens when aggressive simplification (e.g., 95% reduction) is requested on a complex model? The algorithm simplifies as far as the quality constraints allow and reports the actual reduction achieved, even if it falls short of the target.
- How does the system handle meshes with degenerate triangles (zero-area) from the extraction step? Degenerate triangles are removed during simplification as a natural byproduct of edge collapse.
- What happens when simplification is applied to a mesh with per-face colors and the face count changes? Colors are resampled at the new mesh positions from the original color source, not interpolated from the old per-face colors.
- What happens when the user exports to a format that doesn't support color (e.g., STL)? Color resampling is skipped entirely.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide a mesh simplification algorithm based on greedy Quadric Error Metrics (QEM) with an incremental priority queue, replacing the current multi-pass batch approach for the "fast" mode.
- **FR-002**: System MUST support three termination modes: target triangle count, target reduction percentage, and error-bounded (maximum geometric error).
- **FR-003**: System MUST preserve mesh manifoldness during simplification — every edge collapse that would create non-manifold geometry, flip triangles, or open holes MUST be rejected.
- **FR-004**: System MUST work with all three mesh extraction methods: Layered Marching Cubes, Dual Contouring, and Manifold Dual Contouring.
- **FR-005**: System MUST preserve boundary edges — edges on mesh boundaries are either locked or only collapsed along the boundary curve.
- **FR-006**: System MUST detect and prevent triangle normal flips during edge collapse (dot product of old and new normals must remain positive).
- **FR-007**: System MUST detect and prevent zero-volume folds (two triangles sharing an edge with opposite orientations) during edge collapse.
- **FR-008**: System MUST offer a "fast (geometric)" simplification mode that uses pure QEM without GPU evaluation, suitable for rapid exports.
- **FR-009**: System MUST retain the existing "quality (SDF-aware)" simplification mode that evaluates the implicit function on the GPU to measure surface deviation.
- **FR-010**: System MUST resample/reassign per-vertex colors from the original color source (implicit color function) after simplification, so that the simplified mesh has correct colors at its new vertex positions.
- **FR-011**: System MUST provide the simplification options in the mesh export dialog with sensible defaults.
- **FR-012**: System MUST report progress during simplification so that the UI can display a progress indicator.
- **FR-013**: System MUST support cancellation of an in-progress simplification.
- **FR-014**: System MUST remove degenerate triangles (zero-area, duplicate vertex indices) as part of the simplification/compaction step.

### Key Entities

- **Edge Collapse Candidate**: An edge in the mesh being considered for collapse, with an associated error cost computed from vertex quadrics.
- **Vertex Quadric**: A 4×4 symmetric matrix (10 unique doubles) per vertex, representing the sum of plane quadrics from all incident triangles. Used to compute the geometric error of collapsing an edge.
- **Simplification Configuration**: User-specified parameters including termination mode (target count / reduction % / error-bound), algorithm choice (fast geometric / quality SDF-aware), and quality thresholds.
- **Color Source**: The implicit color function used to resample colors at new vertex positions after simplification.

## Assumptions

- The "fast (geometric)" mode uses only geometric information (vertex quadrics, normals) and does not call the GPU SDF evaluator. This makes it dramatically faster but slightly less accurate for preserving implicit surface detail compared to the SDF-aware mode.
- For color resampling, the existing implicit color evaluation pipeline is reused — the simplifier does not need to interpolate colors from the pre-simplification mesh; it queries the color function at the new positions.
- The existing SDF-aware mode is retained as-is or improved incrementally; the primary focus of this feature is adding the fast geometric mode.
- Sensible defaults for the export dialog: simplification off by default, "fast (geometric)" as the default algorithm when enabled, 50% reduction as the default target.
- The simplification operates on the already-extracted indexed triangle set, not on the implicit function directly.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Simplification of a 1M-triangle mesh to 50% completes in under 10 seconds on a typical desktop (4-core CPU, 16GB RAM).
- **SC-002**: The fast geometric mode is at least 5x faster than the current SDF-aware mode for equivalent reduction targets.
- **SC-003**: 100% of simplified meshes pass watertight and manifold validation when the input mesh was watertight and manifold.
- **SC-004**: Simplified meshes can be loaded without errors in PrusaSlicer, OrcaSlicer, and Bambu Studio.
- **SC-005**: Visual quality of a 50%-reduced mesh is indistinguishable from the original at typical 3D printing scale (deviations less than 1 layer height, ~0.2mm).
- **SC-006**: Color deviation on simplified meshes is within perceptual tolerance — average color error (Euclidean distance in RGB) is less than 5% of full range.
- **SC-007**: All three mesh extraction methods produce valid simplified output for the same model.
