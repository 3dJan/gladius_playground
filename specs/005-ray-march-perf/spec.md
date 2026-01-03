# Feature Specification: Ray Marching Performance Optimization

**Feature Branch**: `005-ray-march-perf`  
**Created**: 2026-01-03  
**Status**: Draft  
**Input**: Improve ray marching rendering performance through multi-pass low-resolution buffers, local Lipschitz bounds, enhanced sphere tracing, hierarchical acceleration structures, and optimized GPU utilization.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Faster Progressive High-Quality Rendering (Priority: P1)

As a user editing complex implicit models, I want the progressive high-quality rendering to complete faster so I can see the final result sooner after making changes.

**Why this priority**: The progressive HQ rendering is the most computationally intensive path in the rendering pipeline. Users spend significant time waiting for the final render to complete after parameter edits. Reducing this time directly improves productivity and creative flow.

**Independent Test**: Open a complex model with multiple CSG operations (e.g., `wristsupport.3mf` or a model with beam lattices). Make a parameter change and measure the time from edit to fully rendered HQ image. Success is measurable improvement in wall-clock time.

**Acceptance Scenarios**:

1. **Given** a complex CSG model with 10+ boolean operations, **When** the user triggers a full HQ render, **Then** the complete render time is reduced by at least 30% compared to baseline.
2. **Given** a model with precomputed SDF available, **When** progressive HQ rendering runs, **Then** the average step count per ray is reduced by at least 20%.
3. **Given** a scene with grazing rays (rays nearly parallel to surfaces), **When** rendering completes, **Then** no visual artifacts (missed thin features, Z-fighting) are introduced.

---

### User Story 2 - Smoother Camera Interaction (Priority: P2)

As a user navigating the 3D viewport, I want camera rotation and panning to feel smooth and responsive even on complex models, maintaining at least 30 FPS during interaction.

**Why this priority**: Smooth camera interaction is essential for spatial understanding and usability. The existing low-res preview path already addresses this, but further optimization can improve the experience on complex models or lower-end hardware.

**Independent Test**: Load a complex model and rapidly orbit the camera. Measure frame time variance and ensure no frames exceed 33ms (30 FPS floor).

**Acceptance Scenarios**:

1. **Given** a model with 100k+ SDF evaluations per frame, **When** the user orbits the camera, **Then** the preview frame rate remains above 30 FPS.
2. **Given** the low-res preview mode is active, **When** rendering a preview frame, **Then** the GPU utilization is balanced (no stalls from memory bandwidth or divergence).

---

### User Story 3 - Efficient Mesh SDF Rendering (Priority: P2)

As a user working with imported mesh geometry (represented as Mesh SDFs), I want the rendering of mesh-based implicit surfaces to be as fast as procedural primitives.

**Why this priority**: Mesh SDFs require expensive BVH traversal and distance computations. Optimizing this path enables more complex imported geometry without performance degradation.

**Independent Test**: Load a model containing a Mesh SDF with 50k+ triangles. Measure render time and compare to an equivalent bounding box primitive.

**Acceptance Scenarios**:

1. **Given** a Mesh SDF with voxel acceleration enabled, **When** rendering rays that miss the mesh entirely, **Then** the early-out occurs within 3 SDF evaluations on average.
2. **Given** a Mesh SDF with BVH traversal, **When** a ray intersects the mesh, **Then** the BVH traversal completes with at most O(log N) node visits.

---

### User Story 4 - Reduced GPU Memory Pressure (Priority: P3)

As a user on hardware with limited VRAM, I want the rendering system to use memory efficiently so I can work with larger models without running out of GPU memory.

**Why this priority**: Memory efficiency enables scaling to more complex scenes and improves cache utilization, indirectly improving performance through better memory access patterns.

**Independent Test**: Monitor VRAM usage while loading progressively larger models. Ensure working set size scales sub-linearly with model complexity.

**Acceptance Scenarios**:

1. **Given** a large model, **When** rendering, **Then** the additional memory overhead for acceleration structures is less than 20% of the base SDF storage.
2. **Given** the precomputed SDF 3D texture, **When** accessed during ray marching, **Then** texture cache hit rate exceeds 80% for typical camera views.

---

### Edge Cases

- What happens when a ray grazes a surface at near-zero angles? (Grazing problem must not cause excessive iterations or timeout)
- How does the system handle degenerate SDF values (NaN, infinity, discontinuities)?
- What happens when the precomputed SDF resolution is insufficient for fine details?
- How does performance scale when models contain deeply nested CSG trees (100+ operations)?
- What happens when beam lattice BVH depth exceeds stack limits in the kernel?
- How are rays handled that start inside geometry (e.g., after clipping plane adjustment)?

## Requirements *(mandatory)*

### Functional Requirements

#### Numerical Optimization

- **FR-001**: System MUST implement enhanced sphere tracing with over-relaxation (ω factor between 1.0 and 2.0) for rays traversing empty space
- **FR-002**: System MUST detect and mitigate the grazing problem by switching to smaller step sizes when consecutive steps remain small
- **FR-003**: System MUST maintain binary refinement for surface crossing detection with configurable iteration count (default: 6)
- **FR-004**: System MUST support local Lipschitz bound estimation for adaptive step sizing in CSG operations

#### Multi-Pass Rendering

- **FR-005**: System MUST support a multi-pass rendering strategy where a low-resolution pass pre-computes approximate ray start distances
- **FR-006**: System MUST scale SDF footprints in the low-resolution pass to avoid missing thin features
- **FR-007**: System MUST allow configurable resolution ratios between low-res and high-res passes (default: 1/4 linear, 1/16 pixel count)

#### Hierarchical Acceleration

- **FR-008**: System MUST utilize the precomputed 3D SDF texture to skip empty space during ray marching
- **FR-009**: System MUST support early ray termination when the cached SDF indicates the ray is outside geometry bounds
- **FR-010**: System MUST efficiently traverse Mesh SDF BVH structures with bounded stack depth (maximum 32 levels)

#### GPU Optimization

- **FR-011**: System MUST minimize warp divergence by using uniform control flow where feasible in the main ray march loop
- **FR-012**: System MUST ensure memory access patterns favor spatial locality for the precomputed SDF 3D texture
- **FR-013**: System MUST avoid dynamic memory allocation within ray marching kernels

### Key Entities

- **RayCastResult**: Contains hit status, traveled distance, surface color, and surface type information
- **RenderingSettings**: Configuration flags controlling approximation modes (AM_FULL_MODEL, AM_HYBRID, AM_ONLY_PRECOMPSDF)
- **PreComputedSdf**: 3D texture (typically 256³) caching SDF values for accelerated distance queries
- **DistanceColor**: Combined SDF distance value and material color for each surface sample

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Progressive HQ render completes at least 30% faster on complex CSG models compared to baseline
- **SC-002**: Average ray march step count is reduced by at least 20% for typical viewport renders
- **SC-003**: Camera interaction maintains 30+ FPS on models that previously dropped below 20 FPS during orbit
- **SC-004**: No visual quality regression (artifacts, missed features, incorrect colors) in rendered images compared to baseline
- **SC-005**: Memory overhead for acceleration structures remains below 20% of base model storage
- **SC-006**: Grazing rays converge within 2x the step count of perpendicular rays (no pathological slowdown)

## Assumptions

- The existing async rendering infrastructure (AsyncRenderController, triple buffering) will be preserved and enhanced, not replaced
- OpenCL 1.2+ compatibility must be maintained for cross-platform support
- The precomputed SDF 3D texture approach will continue to be the primary acceleration mechanism for empty space skipping
- Performance improvements should not require changes to the user-visible API or file formats
- The current approximation mode flags (AM_FULL_MODEL, AM_HYBRID, AM_ONLY_PRECOMPSDF) provide the framework for quality/performance tradeoffs
