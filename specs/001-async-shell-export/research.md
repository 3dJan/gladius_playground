# Research: Async Shell-Based Color Export

**Feature**: 001-async-shell-export  
**Date**: 2026-01-07  

## Executive Summary

This research confirms that the async shell export can be implemented by following the existing `ManifoldDualContouringStlExporter` pattern. All required infrastructure (IExporter interface, CancellationToken, ExportState) is already in place and battle-tested.

## Research Tasks Completed

### 1. Existing Async Export Pattern Analysis

**Source**: `ManifoldDualContouringStlExporter.h/.cpp`

The existing async exporter uses:
- `std::future<void>` for background execution
- `std::atomic<State>` for thread-safe state tracking (Idle/Running/Completed/Failed)
- `std::atomic<double>` for progress reporting (0.0 to 1.0)
- `CancellationToken*` pointer for cooperative cancellation
- IExporter interface: `beginExport()`, `advanceExport()`, `finalize()`, `getProgress()`

**Key Pattern**:
```cpp
// beginExport() - launches async work
void beginExport(path, core) {
    m_state = Running;
    m_progress = 0.0;
    m_exportFuture = std::async(std::launch::async, [this, &core] {
        performExport(core);  // Long-running work
    });
}

// advanceExport() - called from UI thread each frame
bool advanceExport(core) {
    if (m_state == Completed || m_state == Failed) {
        return false;  // Done
    }
    return true;  // Still running
}

// Worker thread checks cancellation periodically
if (isCancellationRequested()) {
    m_state = Failed;
    m_errorMessage = "Export cancelled by user";
    return;
}
```

### 2. Shell Generation Flow Analysis

**Source**: `MeshExportDialog::exportShellsTo3mf()` (lines 1131-1275)

Current synchronous flow:
1. Validate document and materials
2. Get precomputed LUTs from ColorToThicknessDialog
3. Create ManifoldDualContouringOptions
4. Call `ExportState::beginExport()` 
5. Create ShellGenerator and call `generateShells()` (BLOCKING - GPU work)
6. For each shell: create Mesh, add faces (BLOCKING - CPU work)
7. Call `MeshWriter3mf::exportMeshesWithMaterialColors()` (BLOCKING - disk I/O)
8. Call `ExportState::endExport()`

**Bottlenecks**:
- `ShellGenerator::generateShells()`: GPU computation per shell (heaviest)
- Mesh face construction: CPU-bound loop
- 3MF file writing: Disk I/O

### 3. Progress Granularity

For N shells, progress can be divided:
- 0% → 5%: Initialization
- 5% → 85%: Shell generation (80% / N per shell)
- 85% → 95%: Mesh construction
- 95% → 100%: File writing

This provides per-shell progress updates matching FR-003.

### 4. Cancellation Points

**Best cancellation points**:
1. Before each shell generation (immediate response)
2. Inside ShellGenerator loop (requires modification - see Alternative below)
3. After mesh construction, before file write

**Decision**: Check cancellation before each shell and after mesh construction. This provides <2s response for typical exports without modifying ShellGenerator internals.

### 5. Error Handling

**Existing pattern from ManifoldDualContouringStlExporter**:
```cpp
try {
    performExport(generator);
    m_state = Completed;
} catch (std::exception const& e) {
    m_errorMessage = e.what();
    m_state = Failed;
}
```

**File cleanup on failure**:
The 3MF writer writes atomically (creates complete file or fails). No partial file cleanup needed.

### 6. Thread Safety Considerations

**GPU Context**: `ComputeCore` and `ManifoldDualContouringGpu` are NOT thread-safe. However, the existing pattern shows they can be used from a background thread if the UI thread does not access them during export.

**Document Access**: Read-only access to Document is safe. The ExportState UI lock prevents modifications during export.

## Decisions Made

| Decision | Rationale | Alternatives Considered |
|----------|-----------|------------------------|
| Create new `ShellExporter` class | Clean separation; follows existing pattern | Modify ShellGenerator to be async (more invasive) |
| Per-shell progress granularity | Matches FR-003; simple to implement | Sub-shell progress (complex; requires ShellGenerator changes) |
| Check cancellation between shells | <2s response; no internal changes needed | Inject cancellation into ShellGenerator (invasive) |
| Write 3MF atomically | Existing lib3mf behavior; no partial file risk | Write shells incrementally (complex) |

## Unresolved Items

None - all technical questions have clear answers from existing patterns.

## References

- [ManifoldDualContouringStlExporter.h](../../gladius/src/io/ManifoldDualContouringStlExporter.h)
- [ManifoldDualContouringStlExporter.cpp](../../gladius/src/io/ManifoldDualContouringStlExporter.cpp)
- [IExporter.h](../../gladius/src/io/IExporter.h)
- [CancellationToken.h](../../gladius/src/io/CancellationToken.h)
- [ExportState.h](../../gladius/src/ui/ExportState.h)
- [ShellGenerator.cpp](../../gladius/src/io/3mf/ShellGenerator.cpp)
- [MeshExportDialog.cpp](../../gladius/src/ui/MeshExportDialog.cpp) (lines 1131-1275)
