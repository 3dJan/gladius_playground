# Feature Specification: Surface-Aligned Color Sampling for Shell Generation

**Feature Branch**: `011-surface-color-shells`  
**Created**: 2026-01-09  
**Status**: Draft  
**Input**: User description: "Fix shell color sampling to use surface colors instead of interior colors for HueForge-style 3D printing"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Accurate Color Reproduction on 3D Surfaces (Priority: P1)

As a user creating a HueForge-style multi-material print from a 3D model with volumetric colors (e.g., an image projected onto a curved surface), I want the generated shell thicknesses to match the colors visible on the model's outer surface, so that the printed result accurately reproduces the intended image/colors when viewed frontlit.

**Why this priority**: This is the core bug fix — currently the system samples colors at arbitrary interior points, which produces incorrect shell thicknesses and fails to reproduce the intended visual appearance. Without this fix, the entire color-to-shell feature is fundamentally broken for realistic use cases.

**Independent Test**: Export a model with an image projected onto a curved 3D surface (e.g., a lithophane image on a sphere or dome). Verify that:
1. The shell thickness at any point corresponds to the surface color at that location (not the interior color)
2. The exported shells, when 3D printed with the configured filament stack, visually reproduce the source image

**Acceptance Scenarios**:

1. **Given** a 3D model with a planar image projection onto a curved surface, **When** shell export is performed, **Then** the thickness at each surface point is computed from the color at that surface point (not the color at interior evaluation points)

2. **Given** a model where the volumetric color varies with depth (e.g., different colors at surface vs interior), **When** shell export is performed, **Then** only the surface color influences the shell thickness

3. **Given** a HueForge-style export of an image on a 3D surface, **When** the result is visually compared to the source image, **Then** the color distribution matches the original image layout

---

### User Story 2 - Consistent Results Across Surface Geometries (Priority: P2)

As a user, I want shell generation to produce correct color reproduction regardless of the model's surface curvature (flat, convex, concave, complex), so that I can apply HueForge-style effects to any 3D geometry.

**Why this priority**: Different surface geometries stress the sampling algorithm differently. Ensuring consistency validates the robustness of the solution.

**Independent Test**: Test shell export on three reference models: a flat plane, a convex dome, and a concave bowl — all with the same projected image. Verify consistent color reproduction across all geometries.

**Acceptance Scenarios**:

1. **Given** a flat surface with an image projection, **When** shell export is performed, **Then** the thickness distribution matches the image colors

2. **Given** a convex surface (sphere, dome) with the same image projection, **When** shell export is performed, **Then** the thickness distribution matches the image colors without distortion from sampling errors

3. **Given** a concave surface (bowl, cavity) with the same image projection, **When** shell export is performed, **Then** the thickness distribution matches the image colors

---

### User Story 3 - Performance Within Acceptable Bounds (Priority: P3)

As a user, I want the corrected shell generation to complete within a reasonable time, so that the fix does not make the feature impractical for real-world use.

**Why this priority**: Correctness is more important than speed, but the solution should not introduce unacceptable performance degradation.

**Independent Test**: Time the shell export for a reference model (e.g., 100k triangle equivalent) and compare to baseline. Performance should be within 3x of the current (incorrect) implementation.

**Acceptance Scenarios**:

1. **Given** a moderately complex model (100k triangles equivalent), **When** shell export is performed, **Then** export completes within 3x the time of the current implementation

2. **Given** a complex model with high-resolution color mapping, **When** shell export is performed, **Then** progress is reported and the operation remains cancellable

---

### Edge Cases

- What happens when the model has sharp edges or creases where surface normal is discontinuous? → The system should still produce valid shells; thickness may interpolate across the discontinuity
- What happens when a surface point has no well-defined outward normal (e.g., at a singularity)? → Fall back to sampling at the evaluation point with a warning
- What happens when the surface color is undefined (model has no volumetric color)? → Skip color-based thickness; use constant thickness or report error (existing behavior)
- What happens when shells would self-intersect due to high curvature relative to shell thickness? → Generate the mathematically correct shell; let the slicer handle overlaps (document limitation)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Shell thickness computation MUST use the color at the nearest surface point (SDF=0), not the color at the interior evaluation point
- **FR-002**: The surface color sampling MUST work correctly for all supported color definition methods (planar projection, triplanar mapping, procedural textures, etc.)
- **FR-003**: The system MUST produce geometrically valid, watertight shell meshes
- **FR-004**: The system MUST correctly handle the full range of supported surface geometries (flat, convex, concave, mixed curvature)
- **FR-005**: Shell generation MUST remain cancellable and report progress during long operations
- **FR-006**: The system SHOULD minimize performance overhead from surface color sampling while maintaining correctness
- **FR-007**: The system MUST produce identical results for identical inputs (deterministic)

### Key Entities

- **Surface Point**: The point on the model's outer surface (SDF=0) that corresponds to a given evaluation point; this is where color should be sampled
- **Evaluation Point**: A point in 3D space where the SDF and color are evaluated during mesh extraction; may be inside the model
- **Thickness LUT**: A precomputed lookup table mapping RGB colors to layer thicknesses; the color input to this LUT must come from surface points
- **Shell Mesh**: A watertight mesh representing one material layer; its shape is determined by surface-sampled color → thickness mapping

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: For a planar image projection on a flat surface, 95%+ of shell thickness values match the expected thickness for the corresponding image pixel color (within measurement tolerance)
- **SC-002**: For an image projection on a curved surface (sphere), the exported shells reproduce the source image with no visible sampling artifacts when printed
- **SC-003**: Shell export for a 100k triangle equivalent model completes within 3x the baseline time
- **SC-004**: All existing shell generation tests continue to pass (no regressions)
- **SC-005**: Generated shell meshes pass watertightness validation (admesh or equivalent)

## Assumptions

- The volumetric color field at the surface (SDF=0) accurately represents the intended visual appearance
- The SDF field is sufficiently accurate near the surface for surface projection to work reliably
- Users accept that extremely thin shells or high-curvature regions may have practical limitations in 3D printing
- The existing filament optical model and thickness solver are correct; this feature addresses only the color sampling location
