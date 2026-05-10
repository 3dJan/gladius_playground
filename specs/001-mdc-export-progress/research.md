# Research: MDC Export Progress Indication

**Date**: 2026-01-06  
**Status**: Complete

## Executive Summary

The MDC export already runs asynchronously but lacks granular progress reporting. The solution is to add a progress callback to `ManifoldDualContouringGpu` following the existing pattern established by `QemMeshSimplifier::setProgressCallback()`.

## Research Tasks

### 1. Current Progress Reporting Architecture

**Finding**: Progress is reported via `std::atomic<double> m_progress` in `ManifoldDualContouringStlExporter`, which is polled by the UI via `getProgress()`. The current implementation only sets progress at two discrete points:

| Location | Progress Value | Phase |
|----------|----------------|-------|
| After bbox computation | 0.25 | Initialization |
| After `generateMesh()` completes | 0.70 | Mesh generation done |
| After `writeMeshToFile()` | implicit 1.0 | File write done |

**Problem**: The `gpuPipeline.generateMesh()` call is blocking and takes most of the export time, but reports no intermediate progress.

### 2. Existing Progress Callback Patterns

**Finding**: The codebase already has a well-established callback pattern:

```cpp
// From MeshSimplification.h
using SimplificationProgressCallback = std::function<void(
    std::size_t currentTriangles,
    std::size_t targetTriangles,
    std::size_t collapsedTriangles)>;

void setProgressCallback(SimplificationProgressCallback callback);
```

**Decision**: Use the same pattern for `ManifoldDualContouringGpu`:
- Define a `MeshGenerationProgressCallback` type
- Add `setProgressCallback()` method
- Call callback at key points during mesh generation

### 3. Progress Reporting Granularity Points

**Finding**: Analysis of `ManifoldDualContouringGpu::generateMesh()` identifies these natural progress points:

| Phase | Sub-operation | Estimated Weight | Granularity |
|-------|---------------|------------------|-------------|
| Initialization | Load kernels, setup | 5% | Single report |
| Octree Build | `constructOctree()` | 20% | Per-depth level |
| Vertex Generation | `generateVertices()` | 25% | Per chunk (if chunking) |
| Index Generation | `generateIndices()` | 20% | Per chunk |
| Post-processing | Sharp features, simplification | 20% | Per-pass |
| File Write | STL/3MF output | 10% | Per-batch (colors) |

For chunked mode, progress can be reported per-chunk, providing excellent granularity.

### 4. Thread Safety Considerations

**Finding**: The callback will be invoked from the background export thread but the progress value is already thread-safe (`std::atomic`). The callback should:
1. Not perform UI operations directly
2. Simply update the atomic progress value
3. Optionally update a status message (needs mutex if string-based)

**Decision**: The callback signature will be:
```cpp
using MeshGenerationProgressCallback = std::function<void(float progress, std::string_view phase)>;
```

For simplicity, the exporter will capture `this` and update `m_progress` atomically. The phase string is optional for status messages.

### 5. Cancellation Support

**Finding**: The current implementation has no cancellation mechanism. The `onExportCancelled()` method in `MeshExportDialog` simply sets flags but the background thread runs to completion.

**Decision**: Add an `std::atomic<bool> m_cancelRequested` flag that can be checked during long operations. This is out of scope for P1 but the callback infrastructure enables it.

### 6. Performance Impact Assessment

**Finding**: Progress callbacks add negligible overhead if:
1. Called infrequently (≤100 times per second)
2. Callback body is minimal (atomic write)
3. No allocations in hot path

**Decision**: Report progress at most once per major operation (octree level, chunk, simplification pass). For a typical export with 8 octree levels and 27 chunks, this means ~50 progress updates - well within acceptable overhead.

## Alternatives Considered

### Alternative 1: Polling-based Progress

Query progress from `ManifoldDualContouringGpu` via a `getProgress()` method.

**Rejected**: Requires the GPU class to track its own progress state, duplicating what the exporter already does. The callback pattern is more flexible and matches existing code.

### Alternative 2: Event-based Progress (Observer Pattern)

Use a full observer/subscriber pattern with progress events.

**Rejected**: Over-engineered for this use case. A single callback is sufficient and simpler.

### Alternative 3: Status Message Updates Only

Update progress bar but not show percentage, just phase names.

**Rejected**: Users expect numeric progress indication. Phase names can be added as enhancement.

## Conclusions

1. **Add callback to ManifoldDualContouringGpu**: Simple `std::function` callback for progress
2. **Report at chunk/level granularity**: Natural progress points exist in the algorithm
3. **Wire through exporter**: Exporter captures callback and updates atomic progress
4. **No UI changes needed**: `MeshExportDialog` already polls `getProgress()`
5. **Follow existing pattern**: Mirror `QemMeshSimplifier::setProgressCallback()` approach
