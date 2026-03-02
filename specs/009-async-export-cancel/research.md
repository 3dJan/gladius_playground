# Research: Async Export Cancellation

**Feature**: 009-async-export-cancel  
**Date**: 2025-01-07

## Current Implementation Analysis

### Problem Statement

When the user clicks Cancel during an export, the UI freezes because:

1. `BaseExportDialog::render()` calls `onExportCancelled()` when Cancel is clicked
2. `MeshExportDialog::onExportCancelled()` calls `finalize()` on the active exporter
3. `finalize()` in `DualContouringStlExporter` and `ManifoldDualContouringStlExporter` calls `m_exportFuture.wait()`
4. This blocks the main thread until the background export completes

### Code Flow (Current)

```
User clicks Cancel
    → BaseExportDialog::render() detects button click
    → onExportCancelled() called synchronously
    → MeshExportDialog::onExportCancelled()
        → m_activeExporter->finalize()  // BLOCKS HERE
            → m_exportFuture.wait()     // Waits for thread to complete
    → resetState()
    → m_visible = false
```

### Affected Files

| File | Role | Current Issue |
|------|------|---------------|
| `IExporter.h` | Interface | No cancel method |
| `ManifoldDualContouringStlExporter.cpp` | GPU exporter | `finalize()` blocks on future |
| `DualContouringStlExporter.cpp` | CPU exporter | `finalize()` blocks on future |
| `MeshExporter.cpp` | Layer-based exporter | `finalize()` resets grid only (fast) |
| `BaseExportDialog.cpp` | UI base | Calls cancel synchronously |
| `MeshExportDialog.cpp` | UI dialog | Calls finalize in onExportCancelled |
| `ExportState.h` | State tracking | Only Exporting/Idle states |

## C++ Cancellation Patterns Research

### Pattern 1: Atomic Cancellation Flag (Recommended)

**Approach**: Use `std::atomic<bool>` checked at regular intervals in worker thread.

**Pros**:
- Simple, minimal overhead
- Lock-free
- Easy to integrate with existing code
- Works well with `std::future`

**Cons**:
- Worker must actively check flag
- Cancellation latency depends on check frequency

**Example**:
```cpp
class CancellationToken {
    std::atomic<bool> m_cancelled{false};
public:
    void request() { m_cancelled.store(true, std::memory_order_release); }
    bool isCancelled() const { return m_cancelled.load(std::memory_order_acquire); }
};
```

### Pattern 2: std::stop_token (C++20)

**Approach**: Use `std::jthread` with built-in `std::stop_token`.

**Pros**:
- Standard library support
- Automatic cleanup

**Cons**:
- Would require replacing `std::async` with `std::jthread`
- More invasive change
- Less flexible than custom token

**Decision**: Not recommended - requires more refactoring than Pattern 1.

### Pattern 3: Condition Variable with Timeout

**Approach**: Worker sleeps with condition variable, wakes on cancel.

**Pros**:
- Immediate wake-up on cancel

**Cons**:
- Export work is CPU/GPU bound, not sleeping
- Adds unnecessary complexity

**Decision**: Not applicable - our workers are actively computing, not waiting.

## Chosen Approach

**Pattern 1: Atomic Cancellation Flag** with these refinements:

1. **CancellationToken class**: Lightweight wrapper around `std::atomic<bool>`
2. **Shared ownership**: Dialog owns token, passes pointer to exporter
3. **Checkpoint strategy**: Check after major operations (GPU kernel dispatch, file writes)
4. **State extension**: Add `Cancelling` phase to ExportState

## Cancellation Checkpoint Locations

### ManifoldDualContouringStlExporter::performExport()

Key operations that can be interrupted:

| Location | After Operation | Latency Impact |
|----------|-----------------|----------------|
| Line ~445 | `generator.updateBBox()` | <100ms |
| Line ~460 | `ManifoldDualContouringGpu::generate()` | 0.5-2s (main target) |
| Line ~520 | `writeMeshToFile()` | Variable |

**Recommendation**: Add check before and after GPU mesh generation, which is the longest operation.

### DualContouringStlExporter

Similar pattern - check after each major step in the export pipeline.

### MeshExporter (Layer-based)

Check after each layer is processed in the main loop.

## Partial File Cleanup Strategy

1. **Write to temporary file**: Export to `.tmp` extension first
2. **Rename on success**: Atomic rename to final path
3. **Delete on cancel**: Remove `.tmp` file if cancellation detected

**Alternative**: Write directly, delete on cancel. Simpler but risks leaving corrupt files if crash during cleanup.

**Decision**: Use direct write with deletion on cancel - simpler, and crash during cleanup is rare edge case.

## Thread Safety Considerations

1. **CancellationToken**: Already thread-safe via atomic
2. **ExportState**: Uses `std::atomic<bool>` already, extend pattern
3. **File operations**: STL/3MF writers are synchronous within worker thread - no race conditions
4. **UI state**: ImGui is single-threaded, all UI updates happen on main thread

## Performance Impact

- **Cancellation check overhead**: Negligible (<1 microsecond per check)
- **Atomic memory order**: Use `memory_order_relaxed` for flag store, `memory_order_acquire` for load
- **No locks required**: Lock-free design

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| Worker misses cancellation check | Low | Multiple checkpoints |
| Partial file left on disk | Medium | Explicit cleanup in finalize |
| Race between complete and cancel | Low | State machine handles transition |
| GPU operation not interruptible | Medium | Check before/after GPU calls |

## References

- C++ Concurrency in Action, 2nd Ed. - Chapter 4: Synchronizing concurrent operations
- CppCon 2019: "Back to Basics: Concurrency" - Arthur O'Dwyer
- Existing codebase: `ExportState.h` uses `std::atomic<bool>` pattern
