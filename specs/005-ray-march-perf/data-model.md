# Data Model: Ray Marching Performance Optimization

**Feature**: 005-ray-march-perf  
**Date**: 2026-01-03  
**Phase**: 1 (Design & Contracts)

## Entities

### RayCastResult (existing, enhanced)

Represents the result of a single ray march operation.

| Field | Type | Description |
|-------|------|-------------|
| hit | float | 1.0 if surface hit, -1.0 if no hit |
| traveledDistance | float | Total distance traveled along ray |
| color | float4 | Surface color at hit point |
| type | float | Surface type identifier (0=platform, 1=model, 3=isolines) |
| edge | float | Edge detection value (unused in optimization) |

**Changes**: None to structure; behavior change for non-convergence (returns background).

---

### RenderingSettings (existing, enhanced)

Configuration flags controlling render behavior. Passed to kernels via `PAYLOAD_ARGS`.

| Field | Type | Description |
|-------|------|-------------|
| approximation | int | Bitmask: AM_FULL_MODEL, AM_HYBRID, AM_ONLY_PRECOMPSDF |
| flags | int | Bitmask: RF_SHOW_BUILDPLATE, RF_CUT_OFF_OBJECT, etc. |
| z_mm | float | Z-axis cut plane position |

**New Flags** (proposed):
| Flag | Value | Description |
|------|-------|-------------|
| AM_USE_DISTANCE_INIT | 0x08 | Enable low-res distance initialization for HQ |
| RF_DEBUG_METRICS | 0x8000 | Enable debug metric collection (dev builds) |

---

### RayMarchMetrics (new)

Per-frame rendering metrics for debug instrumentation.

| Field | Type | Description |
|-------|------|-------------|
| totalRays | uint | Total rays cast this frame |
| totalSteps | uint | Sum of steps across all rays |
| cacheHits | uint | Number of times cachedSdf() returned early |
| nonConverged | uint | Rays that hit maxRaySteps without hitting surface |
| frameTimeNs | uint64 | Total frame render time (nanoseconds) |

**Storage**: GPU buffer for atomic updates; read back to host after frame completion.

---

### DistanceInitBuffer (new)

Texture storing traveled distances from low-res preview for HQ initialization.

| Property | Value |
|----------|-------|
| Format | Single-channel float (CL_R, CL_FLOAT) |
| Resolution | Same as low-res preview (e.g., width/4 × height/4) |
| Access | Read in HQ pass, written in low-res pass |
| Lifecycle | Allocated once per resolution change; reused across frames |

**Usage**:
1. Low-res pass writes `traveledDistance` to buffer
2. HQ pass samples buffer at pixel center (bilinear interpolation)
3. Subtracts safety margin (e.g., 10% or SDF footprint) before using as startDistance

---

## State Transitions

### Ray March Loop States

```
[START] → [MARCHING] → [REFINED] → [HIT] / [NO_HIT]
                ↑           ↓
                └───────────┘ (refinement triggers re-evaluation)
```

| State | Condition | Action |
|-------|-----------|--------|
| START | traveledDistance = startDistance | Initialize from distance buffer or 0 |
| MARCHING | currentAbsDistance >= closeEnough | Advance by step size with adaptive ω |
| REFINED | distanceSignChanged or slopeSignChanged | Binary search refinement (6 iterations) |
| HIT | currentAbsDistance < closeEnough | Return hit=1, final position |
| NO_HIT | traveledDistance > maxTravelDistance OR i >= maxRaySteps | Return hit=-1, background color |

---

## Relationships

```
RenderJob (1) ────────────> (N) RayCastResult
     │
     │ uses
     ▼
RenderingSettings ──────────> RenderProgram
     │
     │ configures
     ▼
DistanceInitBuffer ─────────> rayCast() startDistance
     │
     │ optional
     ▼
RayMarchMetrics ────────────> Debug Overlay
```

## Validation Rules

1. **ω Bounds**: Over-relaxation factor must be in range [1.0, 2.0]
2. **Distance Init Safety**: Sampled distance must be reduced by safety margin before use
3. **Metric Overflow**: Atomic counters must handle overflow gracefully (wrap or clamp)
4. **Buffer Lifecycle**: DistanceInitBuffer must be valid before enabling AM_USE_DISTANCE_INIT
