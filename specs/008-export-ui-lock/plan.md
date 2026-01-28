# Implementation Plan: Export UI Lock

**Branch**: `008-export-ui-lock` | **Date**: 2025-01-06 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/008-export-ui-lock/spec.md`

## Summary

Block all model-modifying UI interactions during mesh export by rendering a semi-transparent overlay on the model editor and extending `ExportState` checks to all input handlers. The existing `ExportState` mechanism already provides atomic state tracking and is used by some components; this feature extends its usage to cover all modification points.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (immediate-mode UI), imgui-node-editor (node graph)  
**Storage**: N/A  
**Testing**: GTest/GMock  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Desktop application  
**Performance Goals**: Overlay rendering < 0.1ms per frame, no impact on export throughput  
**Constraints**: Main thread must remain responsive during export  
**Scale/Scope**: Single desktop application

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Using std::atomic for thread safety |
| II. Test-First Development | ✅ Pass | Unit tests for ExportState, manual tests for UI |
| III. Simplicity First (KISS) | ✅ Pass | Extends existing pattern, no new abstractions |
| IV. Consistent Code Style | ✅ Pass | Following existing conventions |
| V. Documentation | ✅ Pass | Doxygen for new public methods |

## Project Structure

### Documentation (this feature)

\`\`\`text
specs/008-export-ui-lock/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output
\`\`\`

### Source Code (affected files)

\`\`\`text
gladius/src/ui/
├── ExportState.h           # Existing: Add description getter for overlay text
├── ModelEditor.cpp         # Modify: Add overlay rendering, extend blocking
├── ModelEditor.h           # Modify: Add overlay rendering method
├── NodeView.cpp            # Modify: Add export state checks to input handlers
├── NodeView.h              # Modify: Add ExportState* member
└── MainWindow.cpp          # Verify: File operations already blocked

gladius/tests/unittests/
└── ExportState_tests.cpp   # New: Unit tests for ExportState
\`\`\`

**Structure Decision**: This is a modification to the existing UI layer. No new directories needed. The pattern follows existing ExportState usage in ModelEditor, ResourceView, and BeamLatticeView.

## Complexity Tracking

No violations. Design follows existing patterns.
