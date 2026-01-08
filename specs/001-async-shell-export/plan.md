# Implementation Plan: Async Shell-Based Color Export

**Branch**: `001-async-shell-export` | **Date**: 2026-01-07 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-async-shell-export/spec.md`

## Summary

Convert the synchronous shell-based color export (`exportShellsTo3mf()`) to async execution using the `IExporter` interface pattern, matching the existing `ManifoldDualContouringStlExporter` behavior. The new `ShellExporter` class will run shell generation in a background thread, report per-shell progress, support cooperative cancellation via `CancellationToken`, and integrate with `ExportState` for UI locking.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: OpenCL (GPU compute), lib3mf (3MF export), Eigen (math), ImGui (UI)  
**Storage**: N/A (file export only)  
**Testing**: GTest/GMock (unit tests), GPU tests gated by `GLADIUS_RUN_GPU_TESTS=1`  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single desktop application  
**Performance Goals**: 60 FPS UI responsiveness during export; progress updates ≥1Hz  
**Constraints**: Cancel response <2s; no partial/corrupted output files on cancel/failure  
**Scale/Scope**: Typical 3-10 shells per export; meshes up to millions of triangles

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Using `std::future`, `std::atomic`, smart pointers |
| II. Test-First Development | ✅ Pass | Unit tests for ShellExporter class |
| III. Simplicity First (KISS) | ✅ Pass | Reusing existing IExporter pattern |
| IV. Consistent Code Style | ✅ Pass | Following existing exporter conventions |
| V. Documentation | ✅ Pass | Doxygen comments for public API |

## Project Structure

### Documentation (this feature)

```text
specs/001-async-shell-export/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # N/A (no external APIs)
└── tasks.md             # Phase 2 output
```

### Source Code (repository root)

```text
gladius/src/
├── io/
│   ├── 3mf/
│   │   ├── ShellGenerator.h/.cpp         # Existing - generates shell meshes
│   │   └── MeshWriter3mf.h/.cpp          # Existing - writes 3MF files
│   ├── ShellExporter.h                   # NEW - async IExporter for shells
│   ├── ShellExporter.cpp                 # NEW - implementation
│   ├── IExporter.h                       # Existing - base interface
│   ├── CancellationToken.h               # Existing - cancellation mechanism
│   └── ManifoldDualContouringStlExporter.h/.cpp  # Reference pattern
└── ui/
    ├── MeshExportDialog.h/.cpp           # MODIFY - integrate ShellExporter
    └── ExportState.h                     # Existing - UI lock mechanism

gladius/tests/unittests/
├── io/
│   └── ShellExporter_Test.cpp            # NEW - unit tests
```

**Structure Decision**: Single desktop app structure. New `ShellExporter` class in `gladius/src/io/` follows the existing exporter pattern. Modifications to `MeshExportDialog` replace inline sync code with exporter delegation.

## Complexity Tracking

> No violations - design follows existing patterns.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| N/A | - | - |
