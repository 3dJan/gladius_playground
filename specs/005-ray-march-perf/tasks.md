# Tasks: Ray Marching Performance Optimization

**Input**: Design documents from `/specs/005-ray-march-perf/`  
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, quickstart.md ✅

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)

## Path Conventions

- **OpenCL kernels**: `gladius/src/kernel/`
- **Host compute**: `gladius/src/compute/`
- **UI/async rendering**: `gladius/src/ui/`
- **Tests**: `gladius/tests/integrationtests/`, `gladius/tests/unittests/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Add new flags and buffer infrastructure needed by all user stories

- [x] T001 Add AM_USE_DISTANCE_INIT flag (0x10) to ApproximationMode enum in gladius/src/kernel/types.h
- [x] T002 [P] Add RF_DEBUG_METRICS flag (0x8000) to RenderingFlags enum in gladius/src/kernel/types.h
- [x] T003 [P] Define RayMarchMetrics struct in gladius/src/kernel/types.h (totalRays, totalSteps, cacheHits, nonConverged)
- [x] T004 Add DistanceInitBuffer type alias (cl::Image2D float) in gladius/src/ImageRGBA.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Host-side buffer management that MUST be complete before kernel changes

**⚠️ CRITICAL**: Kernel optimizations cannot use distance init or metrics until these are ready

- [x] T005 Add m_distanceInitBuffer member to Rendering class in gladius/src/compute/Rendering.h
- [x] T006 Implement allocateDistanceInitBuffer() in gladius/src/compute/Rendering.cpp (same resolution as low-res preview)
- [x] T007 [P] Add m_metricsBuffer member to Rendering class in gladius/src/compute/Rendering.h
- [x] T008 [P] Implement allocateMetricsBuffer() and readMetricsBuffer() in gladius/src/compute/Rendering.cpp
- [x] T009 Wire distance buffer lifecycle to low-res preview resolution changes in gladius/src/compute/Rendering.cpp

**Checkpoint**: Buffer infrastructure ready - kernel implementation can begin

---

## Phase 3: User Story 1 - Faster Progressive HQ Rendering (Priority: P1) 🎯 MVP

**Goal**: Achieve 30% faster HQ render and 20% fewer ray steps through adaptive over-relaxation and distance initialization

**Independent Test**: Load wristsupport.3mf, trigger HQ render, measure wall-clock time and step count vs baseline

### Implementation for User Story 1

- [x] T010 [US1] Add gradient estimation function estimateGradientMagnitude() using 4-sample tetrahedron in gladius/src/kernel/rendering.cl (implements FR-004 Lipschitz bound estimation)
- [x] T011 [US1] Add adaptive ω calculation based on gradient magnitude (ω = min(1.6, 1.0/gradient), where 1.6 is practical safe limit per research.md) in rayCast() in gladius/src/kernel/rendering.cl
- [x] T012 [US1] Add grazing detection counter (consecutive small steps > 5 forces ω = 1.0) in rayCast() in gladius/src/kernel/rendering.cl
- [x] T013 [US1] Add distanceInitTexture parameter to rayCast() function signature in gladius/src/kernel/rendering.cl
- [x] T014 [US1] Sample distance texture at ray start with bilinear interpolation and safety margin in rayCast() in gladius/src/kernel/rendering.cl
- [x] T015 [US1] Add fallback to startDistance=0 when AM_USE_DISTANCE_INIT is not set or texture invalid in gladius/src/kernel/rendering.cl
- [x] T016 [US1] Update renderScene kernel to accept and pass distance texture to rayCast() in gladius/src/kernel/renderer.cl
- [x] T017 [US1] Write traveledDistance to distance buffer during low-res preview pass in gladius/src/kernel/renderer.cl
- [ ] T018 [US1] Update RenderProgram to set distance texture kernel argument in gladius/src/RenderProgram.cpp
- [ ] T019 [US1] Wire distance buffer from low-res render to HQ render job in gladius/src/ui/RenderWindow.cpp
- [ ] T020 [US1] Enable AM_USE_DISTANCE_INIT flag when valid low-res result available in gladius/src/ui/RenderWindow.cpp

**Checkpoint**: User Story 1 complete - HQ rendering uses adaptive ω and distance initialization

---

## Phase 4: User Story 2 - Smoother Camera Interaction (Priority: P2)

**Goal**: Maintain 30+ FPS during camera orbit on complex models

**Independent Test**: Load complex model, rapidly orbit camera, measure frame time variance (no frames > 33ms)

### Implementation for User Story 2

- [ ] T021 [US2] Apply adaptive ω optimization to low-res preview path (reuse T010-T012 logic) in gladius/src/kernel/rendering.cl
- [ ] T022 [US2] Verify warp divergence minimization in main ray march loop - document any control flow changes in gladius/src/kernel/rendering.cl
- [ ] T023 [US2] Profile and optimize texture cache access patterns for precomputed SDF sampling in gladius/src/kernel/rendering.cl
- [ ] T023a [US2] Verify FR-008: precomputed SDF texture is used for empty space skipping (existing behavior - document/confirm) in gladius/src/kernel/rendering.cl
- [ ] T023b [US2] Verify FR-009: early ray termination when outside geometry bounds (existing behavior - document/confirm) in gladius/src/kernel/rendering.cl

**Checkpoint**: User Story 2 complete - camera interaction maintains 30+ FPS

---

## Phase 5: User Story 3 - Efficient Mesh SDF Rendering (Priority: P2)

**Goal**: Mesh SDF rendering performance matches procedural primitives

**Independent Test**: Load model with 50k+ triangle Mesh SDF, measure render time vs equivalent bbox primitive

### Implementation for User Story 3

- [ ] T024 [US3] Verify early-out path in Mesh SDF BVH for rays missing geometry in gladius/src/kernel/mesh_sdf.cl
- [ ] T025 [US3] Add BVH traversal depth limit (32 levels max) with graceful fallback in gladius/src/kernel/mesh_sdf.cl
- [ ] T026 [US3] Optimize BVH node access pattern for cache locality in gladius/src/kernel/mesh_sdf.cl

**Checkpoint**: User Story 3 complete - Mesh SDF performance competitive with procedural primitives

---

## Phase 6: User Story 4 - Reduced GPU Memory Pressure (Priority: P3)

**Goal**: Memory overhead < 20% of base SDF storage; texture cache hit rate > 80%

**Independent Test**: Monitor VRAM usage while loading progressively larger models

### Implementation for User Story 4

- [ ] T027 [US4] Audit buffer allocations in Rendering class - document memory overhead in gladius/src/compute/Rendering.cpp
- [ ] T028 [US4] Ensure distance buffer uses minimal format (CL_R, CL_FLOAT) in gladius/src/compute/Rendering.cpp
- [ ] T029 [US4] Verify metrics buffer is only allocated in debug builds in gladius/src/compute/Rendering.cpp

**Checkpoint**: User Story 4 complete - memory overhead within budget

---

## Phase 7: Debug Instrumentation (Cross-Cutting)

**Purpose**: FR-014 - Debug metrics exposed via overlay (supports all stories)

- [ ] T030 [P] Add atomic counter increments in rayCast() loop (totalSteps, nonConverged) in gladius/src/kernel/rendering.cl
- [ ] T031 [P] Add atomic counter increment in cachedSdf() when early-out occurs (cacheHits) in gladius/src/kernel/rendering.cl
- [ ] T032 [P] Initialize metrics buffer to zero at frame start in renderScene kernel in gladius/src/kernel/renderer.cl
- [ ] T033 Pass metrics buffer as kernel argument in gladius/src/RenderProgram.cpp
- [ ] T034 Read metrics buffer after frame completion via clFinish() in gladius/src/compute/Rendering.cpp
- [ ] T035 Extend existing debug overlay in RenderWindow::drawDebugInfo() to display step count, cache hits, non-convergence rate in gladius/src/ui/RenderWindow.cpp
- [ ] T036 Gate metrics collection with #ifdef GLADIUS_DEBUG_METRICS in all kernel files

**Checkpoint**: Debug instrumentation complete - metrics visible in overlay

---

## Phase 8: Baseline & Regression Testing

**Purpose**: Establish baselines and ensure no visual quality regression

- [ ] T037 [P] Create baseline benchmark test file in gladius/tests/integrationtests/RayMarchPerf_tests.cpp
- [ ] T038 [P] Add thumbnail rendering benchmark for wristsupport.3mf in gladius/tests/integrationtests/RayMarchPerf_tests.cpp
- [ ] T039 Store baseline metrics (render time, step count) as JSON in specs/005-ray-march-perf/baselines/wristsupport.json
- [ ] T040 Add visual regression test comparing rendered image to golden reference in gladius/tests/integrationtests/RayMarchPerf_tests.cpp
- [ ] T041 Add grazing ray scenario test (ray nearly parallel to surface) in gladius/tests/integrationtests/RayMarchPerf_tests.cpp
- [ ] T042 Add non-convergence behavior test (verify background color returned) in gladius/tests/unittests/Rendering_tests.cpp
- [ ] T042a Add edge case test for degenerate SDF values (NaN, infinity) - verify graceful handling in gladius/tests/unittests/Rendering_tests.cpp
- [ ] T042b Add edge case test for rays starting inside geometry (clipping plane scenario) in gladius/tests/integrationtests/RayMarchPerf_tests.cpp

---

## Phase 9: Polish & Validation

**Purpose**: Final cleanup and verification

- [ ] T043 [P] Add Doxygen comments to new functions (estimateGradientMagnitude, etc.) in gladius/src/kernel/rendering.cl
- [ ] T044 [P] Update rendering_pipeline.md with optimization documentation in docs/architecture/rendering_pipeline.md
- [ ] T045 Run full test suite and verify all success criteria (SC-001 through SC-006)
- [ ] T046 Run quickstart.md validation steps

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup) ──────────────────────────────────────────────────────────────►
                  │
Phase 2 (Foundational) ───────────────────────────────────────────────────────►
                          │
                          ├── Phase 3 (US1: HQ Rendering) ────────────────────►
                          │
                          ├── Phase 4 (US2: Camera) ──────────────────────────►
                          │
                          ├── Phase 5 (US3: Mesh SDF) ────────────────────────►
                          │
                          └── Phase 6 (US4: Memory) ──────────────────────────►
                                                      │
Phase 7 (Debug Instrumentation) ──────────────────────┴───────────────────────►
                                                                  │
Phase 8 (Testing) ────────────────────────────────────────────────┴───────────►
                                                                        │
Phase 9 (Polish) ───────────────────────────────────────────────────────┴─────►
```

### Critical Path (MVP - US1 only)

1. T001–T004 (Setup) — 4 tasks
2. T005–T009 (Foundational) — 5 tasks
3. T010–T020 (US1 Implementation) — 11 tasks
4. T037–T040 (Core Testing) — 4 tasks

**MVP Total**: 24 tasks

### Parallel Opportunities per Phase

| Phase | Parallel Tasks |
|-------|----------------|
| Setup | T002, T003, T004 can run in parallel with T001 |
| Foundational | T007, T008 can run in parallel with T005, T006 |
| US1 | T013-T015 (kernel) can run in parallel with T018-T020 (host) after T016 |
| Debug | T030, T031, T032 can all run in parallel |
| Testing | T037, T038 can run in parallel |

---

## Summary

| Phase | Task Count | Key Files |
|-------|------------|-----------|
| Setup | 4 | rendering.h, types.h, ImageRGBA.h |
| Foundational | 5 | Rendering.h, Rendering.cpp |
| US1 (P1) | 11 | rendering.cl, renderer.cl, RenderProgram.cpp, RenderWindow.cpp |
| US2 (P2) | 5 | rendering.cl |
| US3 (P2) | 3 | mesh_sdf.cl |
| US4 (P3) | 3 | Rendering.cpp |
| Debug | 7 | rendering.cl, renderer.cl, RenderProgram.cpp, RenderWindow.cpp |
| Testing | 8 | RayMarchPerf_tests.cpp, Rendering_tests.cpp |
| Polish | 4 | Documentation, validation |

**Total**: 50 tasks
