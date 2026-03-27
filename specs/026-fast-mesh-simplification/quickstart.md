# Quickstart: Fast Mesh Simplification for Export

**Feature**: 026-fast-mesh-simplification

## What This Feature Does

Adds a fast, PrusaSlicer-class mesh simplification option to Gladius's mesh export pipeline. The new "Fast (Geometric)" mode uses a single-pass greedy QEM algorithm with an incremental priority queue — no GPU required. It runs 5-50x faster than the existing SDF-aware mode while preserving mesh quality and printability.

## How to Use It

### From the Export Dialog

1. Open File → Export → STL/3MF
2. Choose any mesh extraction method
3. Under **Simplification**, select:
   - **Fast (Geometric)** — for speed (new, recommended for most cases)
   - **Quality (SDF-aware)** — for maximum surface fidelity (existing)
4. Choose termination mode:
   - **Target reduction %** — e.g., reduce to 50% of original triangles
   - **Target triangle count** — e.g., reduce to exactly 100,000 triangles
   - **Error-bounded** — simplify as much as possible within error threshold
5. Click Export

### From Code

```cpp
#include "compute/MeshSimplification.h"

// After mesh extraction:
std::vector<Eigen::Vector3f> positions = /* from extraction */;
std::vector<uint32_t> indices = /* from extraction */;

gladius::compute::FastQemConfig config;
config.terminationMode = gladius::compute::SimplificationTerminationMode::TargetReductionPercent;
config.targetReductionPercent = 50.0F;

std::size_t collapsed = gladius::compute::fastQemSimplify(
    positions, indices, config,
    []() { /* check cancellation */ },
    [](int pct) { /* progress 0-100 */ });

// Positions and indices are now simplified in-place.
// Then evaluate colors at new positions if needed.
```

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Single-pass greedy QEM with priority queue | O(n log n) vs O(passes × edges) — dramatically faster |
| Flat adjacency arrays (not hash maps) | Cache-friendly, ~10x fewer allocations |
| Double precision for quadric math | Avoids SVD fallback, ~100x faster per edge |
| Color resampling AFTER simplification | Uses ground-truth color function, no error accumulation |
| Simplification decoupled from extraction method | Works with LMC, DC, and MDC identically |

## Files Changed

| File | Change |
|------|--------|
| `gladius/src/compute/MeshSimplification.h` | Add `FastQemConfig`, `fastQemSimplify()`, `MutablePriorityQueue`, internal data structures |
| `gladius/src/compute/MeshSimplification.cpp` | Implement fast single-pass QEM algorithm |
| `gladius/src/compute/ManifoldDualContouringGpu.h` | Add `QemFast` to compute `SimplificationMethod` |
| `gladius/src/compute/ManifoldDualContouringGpu.cpp` | Wire up `QemFast` in `simplifyMesh()` switch |
| `gladius/src/io/SurfaceExtractionOptions.h` | Add `QemFast` to io `SimplificationMethod`, add `SimplificationTerminationMode` |
| `gladius/src/io/ManifoldDualContouringStlExporter.cpp` | Map new options to compute config |
| `gladius/src/ui/MeshExportDialog.h` | Add UI state for new controls |
| `gladius/src/ui/MeshExportDialog.cpp` | Add "Fast (Geometric)" option and termination mode controls |
| `gladius/tests/unittests/MeshSimplification_tests.cpp` | Add tests for fast QEM algorithm |

## How to Test

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run tests
# Use VS Code task: "Run Gladius Tests"
# Or filter: --gtest_filter="FastQemSimplifier*"
```

### Manual Testing

1. Load a model with complex geometry
2. Export with Fast (Geometric) at 50% reduction
3. Load result in PrusaSlicer — verify no mesh errors
4. Compare visually with unsimplified export
5. For colored models: verify colors match after simplification
