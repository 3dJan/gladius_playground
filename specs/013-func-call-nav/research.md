# Research: FunctionCall Node Double-Click Navigation

**Date**: 2026-01-24  
**Branch**: `013-func-call-nav`

## Bug Analysis

### Current Implementation (Broken)

**Location**: `NodeView::show()` lines 315-337

```cpp
void NodeView::show(NodeBase & baseNode)
{
    header(baseNode);
    content(baseNode);

    // Check for double-click on FunctionCall nodes to navigate to referenced function
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))  // <-- BUG HERE
    {
        // ... navigation logic
    }

    footer(baseNode);
}
```

### Root Cause

`ImGui::IsItemHovered()` checks hover state of the **last drawn ImGui item**, not the node as a whole. After `content()` executes, the "last item" is typically:
- An input field (e.g., `ImGui::InputFloat`)
- A label or text element
- A button or combo box

This means:
1. **Clicking empty space in the node** → Does NOT trigger (no item is hovered)
2. **Clicking an input field** → May trigger, but unpredictably (depends on frame timing and active widget state)

### Correct API

The ImGui Node Editor library provides node-level hover detection:

```cpp
// Available in imgui_node_editor.h
IMGUI_NODE_EDITOR_API NodeId GetHoveredNode();
```

This returns the `NodeId` of the node currently under the mouse cursor, regardless of which internal widget was drawn last.

### Why Detection Must Move to ModelEditor

The `GetHoveredNode()` function queries state from the node editor context. It should be called:
1. **After all nodes are rendered** (so hover state is computed)
2. **Before `ed::End()`** (while editor context is still active)
3. **In ModelEditor** (which owns the editor context)

`NodeView` visits nodes during rendering but doesn't have the right timing or context for this query.

## Navigation History Analysis

### FunctionNavigationHistory Class

**Location**: `gladius/src/ui/FunctionNavigationHistory.h`

The class is well-designed and functional:

| Method | Purpose | Status |
|--------|---------|--------|
| `recordNavigation()` | Add new navigation to history, truncate forward | ✅ Working |
| `canGoBack()` | Check if back navigation available | ✅ Working |
| `canGoForward()` | Check if forward navigation available | ✅ Working |
| `goBack()` | Return previous function ID | ✅ Working |
| `goForward()` | Return next function ID | ✅ Working |
| `reset()` | Initialize with starting function | ✅ Working |

### Mouse Button Handlers

**Location**: `ModelEditor::showAndEdit()` lines 1335-1346

```cpp
if (isHovered())
{
    if (ImGui::IsKeyPressed(ImGuiKey_MouseX1, false))  // Mouse back button
    {
        goBack();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_MouseX2, false))  // Mouse forward button
    {
        goForward();
    }
}
```

**Status**: ✅ Working correctly (uses ImGui key constants for mouse X buttons)

## Fix Strategy

### Proposed Changes

#### 1. Remove Broken Code from NodeView::show()

Delete lines 315-337 that contain the broken `ImGui::IsItemHovered()` check.

#### 2. Add Correct Detection in ModelEditor::showAndEdit()

Insert after group double-click handling (around line 1354), before `ed::End()`:

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

### Edge Cases

| Scenario | Expected Behavior |
|----------|-------------------|
| Double-click on input field | Text selection, NO navigation (input steals focus) |
| Double-click on empty node area | Navigate to function |
| Double-click on non-FunctionCall node | No action |
| Referenced function doesn't exist | No action (functionId = 0 or lookup fails) |
| Already viewing the target function | No action (navigateToFunction returns early) |

## Alternatives Considered

### Alternative 1: Use ImGui Node Editor callbacks

The library may have double-click callbacks via `ed::NodeDoubleClicked()` or similar. However:
- No such callback exists in the current API
- `GetHoveredNode()` + `IsMouseDoubleClicked()` is the documented pattern

**Decision**: Rejected - API doesn't exist

### Alternative 2: Store node bounds and do manual hit testing

Could store each node's screen bounds during render and do manual point-in-rect testing.

**Decision**: Rejected - `GetHoveredNode()` already provides this functionality with less code

### Alternative 3: Fix IsItemHovered by drawing an invisible overlay

Could draw an invisible button over the entire node content area after `content()`.

**Decision**: Rejected - Hacky, interferes with normal node interaction, harder to maintain

## Conclusion

The fix is straightforward:
1. Remove broken code from `NodeView::show()`
2. Add ~20 lines to `ModelEditor::showAndEdit()` using `ed::GetHoveredNode()`
3. Navigation history and mouse buttons already work correctly
