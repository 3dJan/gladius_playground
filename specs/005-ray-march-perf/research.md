# Research: Ray Marching Performance Optimization

**Feature**: 005-ray-march-perf  
**Date**: 2026-01-03  
**Phase**: 0 (Outline & Research)

## Research Tasks

### 1. Adaptive Over-Relaxation (ω) Implementation

**Question**: How to implement per-ray adaptive ω based on SDF gradient?

**Initial Decision**: Use gradient magnitude from finite differences to estimate local Lipschitz bound

**Initial Implementation**:
- SDF gradient magnitude approximates local Lipschitz constant L
- Safe step = distance / L; over-relaxation ω = min(2.0, 1/L) when L < 1
- Gradient computed via 4-sample tetrahedron method

**Problem Discovered (2026-01-03)**:
A/B testing revealed only ~2% step reduction with gradient-based approach. Root cause: well-formed SDFs (e.g., gyroid, CSG unions) have gradient magnitude ≈ 1.0, so ω = 1/gradMag ≈ 1.0 provides no benefit.

**Final Decision**: Use Enhanced Sphere Tracing (Keinert et al. 2014) with overshoot detection

**Final Implementation**:
- Track previous step size (`prevStepSize`) and previous distance (`prevAbsDistance`)
- Use full over-relaxation (ω = 1.6) by default when far from surfaces
- Detect overshoot: if `prevAbsDistance + currentAbsDistance < prevStepSize`, we missed a surface
- On overshoot: backtrack to previous position, retake step with ω = 1.0 (conservative)
- Benefits:
  - Works regardless of SDF gradient characteristics
  - Provides ~19% step reduction on both ideal and non-ideal SDFs
  - Self-correcting: overshoots are detected and fixed automatically

**Results**:
| Model | Baseline (ω=1.0) | Optimized | Step Reduction |
|-------|------------------|-----------|----------------|
| ImplicitGyroid | 111.23 steps/ray | 89.84 steps/ray | **19.2%** |
| SphereInACage | 103.37 steps/ray | 83.69 steps/ray | **19.0%** |

**Alternatives Considered**:
1. Fixed ω per scene → Rejected: doesn't adapt to local geometry complexity
2. User-adjustable ω → Rejected: clarified that automatic is preferred
3. Scene-based presets → Rejected: not adaptive enough for mixed scenes
4. **Gradient-based ω (implemented, then replaced)** → Only ~2% benefit for well-formed SDFs

**Implementation Notes**:
- Overshoot detection: `prevAbsDistance + currentAbsDistance < prevStepSize`
- Backtrack logic reuses previous distance sample (no additional SDF evaluation)
- Conservative stepping (ω = 1.0) only when close to surfaces or after backtrack
- RF_DISABLE_ADAPTIVE_OMEGA flag allows A/B testing in integration tests

---

### 2. Low-Res Preview Reuse for HQ Initialization

**Question**: How to use existing low-res preview distances as HQ ray start distances?

**Decision**: Store traveled distance from low-res render in a texture; sample in HQ pass

**Rationale**:
- Low-res preview already runs during camera movement (`renderLowResPreview()`)
- Store `traveledDistance` in addition to color in low-res result
- HQ pass samples this distance buffer as `startDistance` parameter to `rayCast()`
- Already clarified: no separate pre-pass needed

**Alternatives Considered**:
1. Dedicated multi-pass with separate resolution → Rejected: adds latency, clarified to reuse existing
2. Per-pixel distance buffer in main render → Rejected: doubles memory, low-res sufficient

**Implementation Notes**:
- Low-res buffer is typically 1/4 linear (1/16 pixels)
- Sample with bilinear interpolation for HQ pixels
- Scale distance conservatively (subtract safety margin based on SDF footprint)
- Fallback to `startDistance = 0` when low-res not available

**Files Affected**:
- `gladius/src/kernel/rendering.cl`: Accept distance texture, sample at ray start
- `gladius/src/kernel/renderer.cl`: Pass distance texture to rayCast
- `gladius/src/compute/Rendering.h/cpp`: Manage distance texture lifecycle
- `gladius/src/ui/RenderWindow.cpp`: Wire low-res distance to HQ render job

**Implementation Results (2026-01-04)**:
- Infrastructure fully implemented and tested
- A/B testing on ImplicitGyroid (512x512) showed ~4% overhead, not speedup
- Root cause: Gyroid-like scenes fill most of the visible space with no significant empty regions to skip
- The distance init buffer sampling overhead (plus 10% safety margin) outweighs benefits
- **Conclusion**: Feature is scene-dependent. Disabled by default in RenderWindow but infrastructure retained
- Future work: Add heuristic to enable only for scenes with significant empty space (e.g., single objects)

---

### 3. Grazing Problem Mitigation

**Question**: How to detect and handle grazing rays efficiently?

**Decision**: Track consecutive small steps; reduce ω and increase step count tolerance

**Rationale**:
- Grazing rays produce sequences of small steps (distance ≈ step size)
- Current code already has `nearRangeFactor` for this purpose
- Enhance with explicit grazing detection counter
- When grazing detected, force ω = 1.0 (no relaxation) and allow more steps

**Alternatives Considered**:
1. Switch to analytic intersection → Rejected: not general for arbitrary SDFs
2. Lipschitz bound from gradient → Partially adopted: combines with this

**Implementation Notes**:
- Define grazing threshold: 5+ consecutive steps where `step < 2 * closeEnough`
- Reset counter on larger steps
- Success metric: SC-006 (grazing rays converge within 2x perpendicular steps)

---

### 4. Debug Metrics Instrumentation

**Question**: How to expose per-frame rendering metrics in debug overlay?

**Decision**: Add atomic counters in kernel; read back after frame; display in ImGui

**Rationale**:
- Requirement FR-014: debug builds expose step count, cache hit rate, non-convergence
- OpenCL supports atomic operations for counters
- Minimal overhead when disabled (counters not written)
- Existing ImGui overlay can be extended for metrics display

**Alternatives Considered**:
1. CPU-side estimation → Rejected: inaccurate for GPU execution
2. External profiler only → Rejected: need in-app visibility per clarification

**Implementation Notes**:
- Conditional compilation: `#ifdef GLADIUS_DEBUG_METRICS`
- Counters: `totalSteps`, `totalRays`, `cacheHits`, `nonConverged`
- Host reads counters after `clFinish()`
- Display in existing debug panel (F3 or similar toggle)

**Files Affected**:
- `gladius/src/kernel/rendering.cl`: Atomic counter updates
- `gladius/src/kernel/renderer.cl`: Initialize/pass counter buffer
- `gladius/src/compute/Rendering.cpp`: Allocate/read counter buffer
- `gladius/src/ui/DebugPanel.cpp` (or similar): Display metrics

---

### 5. Baseline Benchmarking Setup

**Question**: How to establish reproducible performance baselines?

**Decision**: Use thumbnail rendering on existing 3mf test files; store results in git

**Rationale**:
- Per clarification: use existing integration test 3mf files
- Thumbnail rendering provides consistent, deterministic viewport
- Store baseline as JSON with model name, camera params, timing, step counts
- CI can compare current run against stored baseline

**Alternatives Considered**:
1. Fixed commit baseline → Rejected: clarified per-model baselines preferred
2. Manual benchmarking → Rejected: need reproducible automation

**Implementation Notes**:
- Test files: `wristsupport.3mf`, beam lattice models from `gladius/examples/`
- Capture: render time (ms), total steps, average steps/ray, non-convergence count
- Store in `specs/005-ray-march-perf/baselines/` as JSON
- Integration test compares against baseline with tolerance (e.g., 5% variance)

---

### 6. Shading Optimization Analysis (2026-01-03)

**Question**: Can we reduce shading cost by optimizing normal calculation and AO?

**Implementation**:
- Added `surfaceNormalFast()` using precomputed SDF texture gradient (6 texture samples vs 4 model evals)
- Reduced AO iterations for preview mode (4 vs 8)
- Soft shadows already skipped in preview mode

**Results (ImplicitGyroid 512x512)**:
| Mode | Render Time | Speedup |
|------|-------------|---------|
| Preview (optimized shading) | 27.28 ms | 1.00x |
| HQ (full shading) | 27.21 ms | baseline |

**Analysis**: 
For complex SDFs like gyroid, ray marching dominates render time (~95%+). Shading optimizations
provide minimal impact because:
1. Gyroid requires ~90 steps per ray (high ray march cost)
2. Shading only runs once per hit pixel
3. The ratio of ray marching to shading is heavily weighted toward marching

**Conclusion**: Shading optimizations provide minimal benefit for complex SDFs.
The primary performance lever remains ray step reduction (Enhanced Sphere Tracing).
For simpler SDFs where shading is a larger fraction of total cost, these optimizations
would provide more noticeable improvement.

**Files Added/Modified**:
- `gladius/src/kernel/rendering.cl`: Added `surfaceNormalFast()`, optimized `calcAmbientOcclusion()` — **REVERTED**
- `gladius/tests/integrationtests/RayMarchPerf_tests.cpp`: Added `ShadingOptimization_PreviewVsHQ_Timing` benchmark — **REMOVED**

**Production Decision**: Shading optimizations reverted as they provide no measurable benefit for the common case.

---

## Production-Ready Status (2026-01-04)

### Optimizations Kept (Production Ready)

| Optimization | Status | Impact | Details |
|--------------|--------|--------|---------|
| Enhanced Sphere Tracing | ✅ PRODUCTION | 19% step reduction | Overshoot detection with backtracking (ω = 1.6) |
| RF_DISABLE_ADAPTIVE_OMEGA | ✅ PRODUCTION | A/B testing | Flag for performance benchmarking |
| Metrics kernel | ✅ PRODUCTION | Debug diagnostics | renderSceneWithMetrics() for step counting |

### Optimizations Reverted/Removed

| Optimization | Status | Reason |
|--------------|--------|--------|
| Distance Init Buffer | ⚠️ REMOVED (code cleaned) | 4% overhead for dense SDFs; infrastructure kept |
| surfaceNormalFast() | ❌ REVERTED | No measurable impact; ray marching dominates |
| Optimized AO iterations | ❌ REVERTED | No measurable impact; ray marching dominates |
| ShadingOptimization test | ❌ REMOVED | Test confirmed no benefit |
| RenderWithDistanceInit test | ❌ REMOVED | Test showed overhead, not speedup |

### Key Learnings

1. **Ray marching dominates for complex SDFs**: For scenes like gyroid with ~90 steps/ray, 
   shading represents <5% of total render time. Optimizing shading provides negligible benefit.

2. **Enhanced Sphere Tracing works universally**: Unlike gradient-based adaptive ω which only 
   helps non-ideal SDFs (2% benefit), the overshoot detection approach provides consistent 
   19% step reduction on all tested models.

3. **Distance init is scene-dependent**: Works theoretically but the overhead outweighs benefits 
   for dense SDF scenes. Would need scene heuristics to enable selectively.

### Files Modified (Production)

- `gladius/src/kernel/rendering.cl`: Enhanced Sphere Tracing in rayCast()
- `gladius/src/kernel/types.h`: RF_DISABLE_ADAPTIVE_OMEGA flag
- `gladius/tests/integrationtests/RayMarchPerf_tests.cpp`: A/B comparison tests

### Validation

All 518 unit tests pass. Integration tests confirm:
- **ImplicitGyroid**: 19.2% step reduction (111.23 → 89.84 steps/ray)
- **SphereInACage**: 19.0% step reduction (103.37 → 83.69 steps/ray)

---

## Unknowns Resolved

| Item | Resolution |
|------|------------|
| Over-relaxation vs correctness tradeoff | Adaptive per-ray ω based on gradient |
| Ray non-convergence behavior | Soft fail with background color; debug viz optional |
| Baseline methodology | Per-model baselines using 3mf test files + thumbnail rendering |
| Instrumentation level | Per-frame metrics via debug overlay (dev builds) |
| Multi-pass triggering | Reuse existing low-res preview (no separate pass) |

## Phase 0 Complete

All NEEDS CLARIFICATION items resolved. Ready for Phase 1: Design & Contracts.
