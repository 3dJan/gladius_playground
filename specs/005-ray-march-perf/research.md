# Research: Ray Marching Performance Optimization

**Feature**: 005-ray-march-perf  
**Date**: 2026-01-03  
**Phase**: 0 (Outline & Research)

## Research Tasks

### 1. Adaptive Over-Relaxation (ω) Implementation

**Question**: How to implement per-ray adaptive ω based on SDF gradient?

**Decision**: Use gradient magnitude from finite differences to estimate local Lipschitz bound

**Rationale**:
- SDF gradient magnitude approximates local Lipschitz constant L
- Safe step = distance / L; over-relaxation ω = min(2.0, 1/L) when L < 1
- Gradient computed via 4-sample tetrahedron method (already in `surfaceNormal()`)
- Low cost: reuse distance samples from marching loop

**Alternatives Considered**:
1. Fixed ω per scene → Rejected: doesn't adapt to local geometry complexity
2. User-adjustable ω → Rejected: clarified that automatic is preferred
3. Scene-based presets → Rejected: not adaptive enough for mixed scenes

**Implementation Notes**:
- Compute gradient every N steps (e.g., 5) to amortize cost
- Cache gradient magnitude for consecutive steps
- Clamp ω to [1.0, 1.6] for safety (literature suggests 1.6 optimal for most SDFs)

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
