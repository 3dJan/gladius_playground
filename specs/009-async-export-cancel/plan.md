# Implementation Plan: Async Export Cancellation

**Branch**: `009-async-export-cancel` | **Date**: 2025-01-07 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/009-async-export-cancel/spec.md`

## Summary

Make export cancellation responsive by providing instant UI feedback and aborting background export threads cooperatively. Currently, clicking Cancel calls `finalize()` which blocks on `m_exportFuture.wait()` until the export thread completes. The solution adds a `CancellationToken` that the export thread checks periodically, allowing it to exit early, while the UI immediately transitions to "Cancelling..." state without blocking.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (UI), OpenCL (GPU compute), OpenVDB (grid processing), std::future/std::atomic (threading)  
**Storage**: File system (STL/3MF export)  
**Testing**: GTest/GMock (unit tests), integration tests with `GLADIUS_RUN_GPU_TESTS=1`  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single desktop application  
**Performance Goals**: Cancel feedback <16ms, export abort <2 seconds, UI 30+ fps during cancellation  
**Constraints**: Must not block main thread, must cleanup partial files  
**Scale/Scope**: 4 exporter classes, 2 dialog classes, 1 state class

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Using `std::atomic`, `std::future`, smart pointers |
| II. Test-First Development | ✅ PASS | Will add unit tests for CancellationToken, integration tests for cancel flow |
| III. Simplicity First (KISS/DRY/YAGNI) | ✅ PASS | Minimal changes: add token, check in loops |
| IV. Consistent Code Style | ✅ PASS | Following existing naming and formatting |
| V. Documentation and Comments | ✅ PASS | Doxygen comments for new public APIs |

**No violations to justify.**

## Project Structure

### Documentation (this feature)

```text
specs/009-async-export-cancel/
├── plan.md              # This file
├── research.md          # Phase 0 output (cancellation patterns)
├── data-model.md        # Phase 1 output (CancellationToken design)
├── quickstart.md        # Phase 1 output (implementation guide)
└── tasks.md             # Phase 2 output (NOT created by /speckit.plan)
```

### Source Code (files to modify/create)

```text
gladius/src/
├── io/
│   ├── CancellationToken.h          # NEW: Thread-safe cancellation signal
│   ├── IExporter.h                  # MODIFY: Add setCancellationToken()
│   ├── ManifoldDualContouringStlExporter.h   # MODIFY: Store token
│   ├── ManifoldDualContouringStlExporter.cpp # MODIFY: Check token in performExport
│   ├── DualContouringStlExporter.h  # MODIFY: Store token
│   └── DualContouringStlExporter.cpp # MODIFY: Check token in loops
├── ui/
│   ├── ExportState.h                # MODIFY: Add Cancelling phase enum
│   ├── MeshExportDialog.h           # MODIFY: Own CancellationToken member
│   └── MeshExportDialog.cpp         # MODIFY: Non-blocking cancel, pass token, cleanup

gladius/tests/
├── unittests/
│   ├── CancellationToken_tests.cpp  # NEW: Unit tests for token
│   └── ExportState_tests.cpp        # MODIFY: Test Cancelling state
└── integrationtests/
    └── ExportCancellation_tests.cpp # NEW: Integration tests for cancel flow
```

**Scope clarification**: MeshExporter (layer-based) is not in scope for this feature as it uses a different export pattern. BaseExportDialog is not modified; all cancel logic is in MeshExportDialog.

## Key Design Decisions

### 1. CancellationToken Pattern

Use a lightweight `std::atomic<bool>` wrapper that can be shared between UI and worker threads:

```cpp
class CancellationToken {
public:
    void requestCancellation() { m_cancelled.store(true); }
    [[nodiscard]] bool isCancelled() const { return m_cancelled.load(); }
    void reset() { m_cancelled.store(false); }
private:
    std::atomic<bool> m_cancelled{false};
};
```

### 2. ExportState Extension

Add `Cancelling` state to the existing enum:

```cpp
enum class ExportPhase { Idle, Exporting, Cancelling };
```

### 3. Non-Blocking Cancel Flow

1. UI click → immediately set `ExportPhase::Cancelling`, update button text
2. Call `m_cancellationToken.requestCancellation()`
3. Do NOT call `finalize()` synchronously
4. Worker thread checks token periodically, exits early
5. `advanceExport()` returns false when worker exits
6. Normal completion path handles cleanup

### 4. Cancellation Checkpoints

Insert checks in these locations:
- `ManifoldDualContouringStlExporter::performExport()` - before/after GPU operations
- `DualContouringStlExporter` - in the export loop
- `MeshExporter` layer processing loops

### 5. Partial File Cleanup

On cancellation:
1. Check if output file exists and is incomplete
2. Delete partial file asynchronously (post-cancellation)
3. Log cleanup success/failure

## Complexity Tracking

> No Constitution violations to justify.

| Item | Justification |
|------|--------------|
| N/A | Design follows KISS principle with minimal additions |
