# Implementation Plan: FunctionCall Node Double-Click Navigation

**Branch**: `013-func-call-nav` | **Date**: 2026-01-24 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/013-func-call-nav/spec.md`

## Summary

Fix broken double-click navigation on FunctionCall nodes to jump to the referenced function, and ensure browser-like back/forward navigation via mouse X1/X2 buttons works correctly.

**Root Cause**: The existing implementation in `NodeView::show()` uses `ImGui::IsItemHovered()` which checks the *last drawn ImGui widget* (typically an input field inside the node), not the node's bounding box. The fix is to move detection to `ModelEditor::showAndEdit()` and use the ImGui Node Editor API `ed::GetHoveredNode()`.

**Navigation History**: The `FunctionNavigationHistory` class and mouse button handlers already work correctly.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui, ImGui Node Editor (`ed::` namespace)  
**Storage**: N/A  
**Testing**: GTest/GMock  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Single desktop application  
**Performance Goals**: 60 fps UI responsiveness  
**Constraints**: Must not interfere with text input fields inside nodes  
**Scale/Scope**: Bug fix with minimal code changes (~30 lines modified)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Requirement | Status | Notes |
|-------------|--------|-------|
| Modern C++ (C++11+) | ✅ PASS | Using C++20 |
| camelCase functions, PascalCase classes | ✅ PASS | Existing patterns followed |
| Allman braces, 4-space indent | ✅ PASS | Matches existing code |
| Files <400 lines | ✅ PASS | Only modifying existing files |
| Build via VS Code tasks | ✅ PASS | Standard workflow |
| GTest/GMock tests | ⚠️ N/A | This is a UI interaction fix; manual testing primary |
| KISS/DRY/YAGNI | ✅ PASS | Minimal change, reuses existing infrastructure |

## Project Structure

### Documentation (this feature)

```text
specs/013-func-call-nav/
├── plan.md              # This file
├── research.md          # Bug analysis and API research
├── data-model.md        # No new data structures (lightweight)
├── quickstart.md        # Manual testing instructions
└── checklists/
    └── requirements.md  # Verification checklist
```

### Source Code (files to modify)

```text
gladius/src/ui/
├── NodeView.cpp           # REMOVE broken double-click code (lines 315-335)
├── ModelEditor.cpp        # ADD proper double-click detection (~line 1354)
└── FunctionNavigationHistory.h/cpp  # NO CHANGES (already correct)
```

## Complexity Tracking

No constitution violations. This is a targeted bug fix with minimal scope.
