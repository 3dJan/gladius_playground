# Implementation Plan: Async Graph Editing

**Branch**: `028-async-graph-editing` | **Date**: 2026-04-15 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/028-async-graph-editing/spec.md`

## Summary

Move all expensive synchronous operations triggered by structural graph edits (node add/delete, link create/delete, paste, extract-to-function) off the UI thread. Currently, `Model::updateTypes()`, `Assembly::updateInputsAndOutputs()`, `Document::updateParameterRegistration()`, and the Assembly deep copy for undo all execute synchronously on the UI thread, causing 7–80 ms stalls on complex models. The approach is to split the structural update pipeline into two phases: (1) a fast UI-thread phase that applies the visual edit and increments an edit epoch, and (2) a background phase that performs type inference, parameter registration, validation, and compilation on a snapshot of the Assembly. Coalescing and epoch-based cancellation ensure only the latest graph state is compiled.

## Technical Context

**Language/Version**: C++20 (Clang)  
**Primary Dependencies**: ImGui (UI), OpenCL 1.2+ (GPU compute), libcoro (C++20 coroutines), lib3mf (file I/O)  
**Storage**: N/A (in-memory Assembly graph + OpenCL GPU buffers)  
**Testing**: GTest/GMock  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single desktop application  
**Performance Goals**: Graph edit UI response < 16 ms (60 fps); background pipeline start < 100 ms  
**Constraints**: Single UI thread (ImGui); Assembly/Model/Node have no internal locking; existing `refreshWorker()` uses `std::async` with `std::future<void>`  
**Scale/Scope**: Models with 100+ nodes, 10+ functions

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | PASS | Uses C++20 coroutines, smart pointers, RAII lock guards, move semantics |
| II. Test-First Development | PASS | Unit tests for coalescing, epoch cancellation, snapshot correctness |
| III. Simplicity First | PASS | Extends existing `refreshWorker()` pattern rather than introducing new framework. Snapshot-based approach is simpler than fine-grained locking |
| IV. Consistent Code Style | PASS | Follows existing naming/formatting conventions |
| V. Documentation and Comments | PASS | New async pipeline documented; public APIs get Doxygen comments |
| VI. UI Responsiveness | PASS | This feature's primary goal — moves all blocking structural operations off UI thread |

All gates pass. No violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/028-async-graph-editing/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (affected files)

```text
gladius/src/
├── Document.h/.cpp                      # refreshWorker, structural update dispatch
├── ui/
│   ├── MainWindow.cpp                   # nodeEditor() callback — remove sync operations
│   └── ModelEditor.cpp                  # showAndEdit() — defer undo snapshot, updateTypes
├── nodes/
│   ├── Assembly.h/.cpp                  # snapshot support, updateInputsAndOutputs
│   ├── Model.h/.cpp                     # updateTypes, graph operations
│   └── History.h/.cpp                   # async-safe undo snapshots
└── ui/render/
    └── AsyncRenderController.h/.cpp     # epoch pattern reference (existing)

gladius/tests/
├── StructuralUpdatePipelineTests.cpp    # NEW: coalescing, epoch cancellation tests
└── AssemblySnapshotTests.cpp            # NEW: snapshot correctness tests
```

**Structure Decision**: All changes are in existing directories under `gladius/src/` and `gladius/tests/`. No new directories needed. The pattern extends the existing `Document::refreshWorker()` infrastructure.

## Complexity Tracking

No constitution violations. No complexity justification needed.
