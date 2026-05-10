# Quickstart: Ray Marching Performance Optimization

**Feature**: 005-ray-march-perf  
**Date**: 2026-01-03  
**Phase**: 1 (Design & Contracts)

## Prerequisites

- Gladius built and running with GPU support
- GTest/GMock for unit tests
- Existing 3mf test files (e.g., `wristsupport.3mf`)

## Implementation Order

### Phase 1: Core Ray Marching Optimization (P1 - Faster HQ Rendering)

1. **Adaptive Over-Relaxation** (FR-001)
   - File: `gladius/src/kernel/rendering.cl`
   - Add gradient estimation using 4-sample tetrahedron method
   - Compute ω = min(1.6, 1.0/gradientMagnitude) when gradient < 1
   - Apply ω multiplier to step size in main loop

2. **Grazing Problem Mitigation** (FR-002)
   - File: `gladius/src/kernel/rendering.cl`
   - Add counter for consecutive small steps
   - When counter > 5, force ω = 1.0 (no relaxation)
   - Reset counter when step exceeds threshold

3. **Low-Res Distance Initialization** (FR-005, FR-006, FR-007)
   - Files: `rendering.cl`, `renderer.cl`, `Rendering.cpp`, `RenderWindow.cpp`
   - Add distance texture output to low-res preview pass
   - Sample distance texture in HQ pass as startDistance
   - Apply safety margin (subtract SDF footprint estimate)
   - Fallback to 0 when texture unavailable

### Phase 2: Debug Instrumentation (P2 - Observability)

4. **Debug Metrics Collection** (FR-014)
   - Files: `rendering.cl`, `renderer.cl`, `Rendering.cpp`
   - Add atomic counter buffer for metrics
   - Conditional compilation for debug builds
   - Collect: totalSteps, cacheHits, nonConverged

5. **Debug Overlay Display**
   - File: `gladius/src/ui/` (appropriate panel file)
   - Read metrics buffer after frame complete
   - Display in ImGui panel (toggle with debug key)

### Phase 3: Baseline & Testing

6. **Baseline Benchmark Infrastructure**
   - File: `gladius/tests/integrationtests/` (new test file)
   - Create performance test using thumbnail rendering
   - Capture baseline metrics as JSON
   - Compare subsequent runs against baseline

7. **Regression Tests**
   - Ensure no visual quality regression (SC-004)
   - Test grazing ray scenarios
   - Test non-convergence behavior

## Key Files

| File | Primary Changes |
|------|-----------------|
| `gladius/src/kernel/rendering.cl` | Adaptive ω, grazing detection, distance init sampling, metrics |
| `gladius/src/kernel/renderer.cl` | Pass distance texture, initialize metrics buffer |
| `gladius/src/kernel/rendering.h` | New flags (AM_USE_DISTANCE_INIT, RF_DEBUG_METRICS) |
| `gladius/src/compute/Rendering.cpp` | Manage distance/metrics buffers |
| `gladius/src/ui/RenderWindow.cpp` | Wire low-res distance to HQ job |

## Build & Test

```bash
# Build (use VS Code task, not direct cmake)
# Task: "Build ALL (linux-releaseWithDebug)"

# Run tests
# Task: "Run Gladius Tests (linux-releaseWithDebug)"

# Run specific benchmark test (after implementation)
cd gladius && ctest --preset ReleaseWithDebug -R RayMarchPerf
```

## Success Verification

| Criterion | Metric | Target |
|-----------|--------|--------|
| SC-001 | HQ render time | ≥30% reduction |
| SC-002 | Avg steps/ray | ≥20% reduction |
| SC-003 | Camera FPS | 30+ on complex models |
| SC-004 | Visual quality | No regression |
| SC-006 | Grazing rays | ≤2x perpendicular step count |

## Notes

- Constitution compliance: All code follows C++20/OpenCL 1.2 standards
- Testing approach: Integration tests with existing 3mf files; unit tests for new utilities
- No API changes: Internal optimization only; user-facing behavior unchanged
