# Tasks: FunctionCall Node Double-Click Navigation

**Input**: Design documents from `/specs/013-func-call-nav/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Tests are NOT required for this feature (UI interaction bug fix, manual testing per quickstart.md)

## Summary

| Phase | Task Count | Description |
|-------|------------|-------------|
| Phase 1 | 0 | N/A - No setup needed |
| Phase 2 | 1 | Remove broken code |
| Phase 3 | 1 | Implement fix (US1, US4) |
| Phase 4 | 0 | N/A - US2, US3 already work |
| Phase 5 | 2 | Validation |
| **Total** | **4** | |

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization

*No tasks - existing project, no new dependencies*

---

## Phase 2: Foundational (Remove Broken Code)

**Purpose**: Remove the broken double-click detection that interferes with proper implementation

- [X] T001 Remove broken double-click code from `NodeView::show()` in gladius/src/ui/NodeView.cpp (lines 315-335)

**Checkpoint**: Broken code removed; no double-click navigation currently active

---

## Phase 3: User Story 1 & 4 - Double-Click Navigation (Priority: P1, P3)

**Goal**: Double-click on FunctionCall or FunctionGradient nodes navigates to the referenced function

**Independent Test**: Open a model with FunctionCall nodes, double-click on node background → editor switches to show referenced function

### Implementation

- [X] T002 Add double-click detection using `ed::GetHoveredNode()` after node group handling (~line 1354) in gladius/src/ui/ModelEditor.cpp

**Details for T002**:
```cpp
// Handle double-click on FunctionCall/FunctionGradient nodes to navigate
ed::NodeId hoveredNodeId = ed::GetHoveredNode();
if (hoveredNodeId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
{
    auto nodeId = static_cast<nodes::NodeId>(hoveredNodeId.Get());
    auto* node = m_currentModel->getNode(nodeId);
    if (node)
    {
        nodes::ResourceId functionId = 0;
        if (auto* fc = dynamic_cast<nodes::FunctionCall*>(node))
        {
            functionId = fc->getFunctionId();
        }
        else if (auto* fg = dynamic_cast<nodes::FunctionGradient*>(node))
        {
            fg->resolveFunctionId();
            functionId = fg->getFunctionId();
        }

        if (functionId != 0)
        {
            navigateToFunction(functionId);
        }
    }
}
```

**Checkpoint**: Double-click navigation on FunctionCall and FunctionGradient nodes works

---

## Phase 4: User Story 2 & 3 - Mouse Back/Forward Navigation (Priority: P1, P2)

**Goal**: Mouse back/forward buttons navigate history

**Status**: ✅ ALREADY WORKING (see research.md)

*No tasks - existing implementation in ModelEditor::showAndEdit() lines 1335-1346 is correct*

**Checkpoint**: Back/forward navigation with mouse X1/X2 buttons works

---

## Phase 5: Polish & Validation

**Purpose**: Verify complete feature works as specified

- [X] T003 Build project using "Build ALL (linux-releaseWithDebug)" task
- [ ] T004 Run manual tests per quickstart.md validation checklist

---

## Dependencies & Execution Order

```
T001 (remove broken code)
  ↓
T002 (add correct detection)
  ↓
T003 (build)
  ↓
T004 (manual validation)
```

All tasks are sequential - no parallel opportunities in this small fix.

---

## Implementation Strategy

### Single Developer Flow

1. **T001**: Remove broken code from NodeView.cpp
2. **T002**: Add new detection in ModelEditor.cpp
3. **T003**: Build and fix any compile errors
4. **T004**: Manual testing using quickstart.md checklist

### Estimated Time

- T001: 5 minutes
- T002: 15 minutes
- T003: 5 minutes (build time)
- T004: 10 minutes

**Total**: ~35 minutes

---

## Notes

- This is a targeted bug fix, not a new feature
- Navigation history and mouse button handlers are already working correctly
- The only code change is relocating detection from NodeView to ModelEditor with correct API
- Manual testing is primary validation method (UI interaction)
