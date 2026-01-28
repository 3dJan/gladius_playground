# Research: Export UI Lock

**Feature**: 008-export-ui-lock  
**Date**: 2025-01-06

## 1. Existing ExportState Mechanism

### Decision
Use the existing `ExportState` class as the foundation for UI locking.

### Rationale
- Already provides atomic `std::atomic<bool>` for thread-safe state tracking
- Already used by `MainWindow`, `ModelEditor`, `ResourceView`, `BeamLatticeView`
- RAII guard (`ExportGuard`) ensures proper cleanup on export completion/failure
- No need to introduce new synchronization primitives

### Implementation Details
```cpp
// ExportState.h - existing implementation
class ExportState
{
    std::atomic<bool> m_exportInProgress{false};
    std::string m_exportDescription;
    // ...
    bool isExportInProgress() const;
    void beginExport(std::string description);
    void endExport();
};
```

### Alternatives Considered
1. **New mutex-based lock class** - Rejected: adds complexity, ExportState already thread-safe
2. **Global flag** - Rejected: violates encapsulation, ExportState already singleton-like

---

## 2. ImGui Overlay Rendering Pattern

### Decision
Use `ImGui::GetWindowDrawList()` with `AddRectFilled()` after `ed::End()` to render the overlay within the Model Editor window.

### Rationale
- Renders overlay on top of node editor content but within the window
- Does not require a separate fullscreen window
- Follows ImGui's immediate-mode paradigm
- Minimal performance impact (single draw call)

### Implementation Details
```cpp
// After ed::End(), before ImGui::End()
if (m_exportState && m_exportState->isExportInProgress())
{
    ImVec2 const windowPos = ImGui::GetWindowPos();
    ImVec2 const windowSize = ImGui::GetWindowSize();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Semi-transparent dark overlay
    drawList->AddRectFilled(
        windowPos,
        ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
        IM_COL32(0, 0, 0, 180)  // RGBA: black at ~70% opacity
    );
    
    // Centered text
    char const* message = "Export in progress...";
    ImVec2 textSize = ImGui::CalcTextSize(message);
    ImVec2 textPos(
        windowPos.x + (windowSize.x - textSize.x) / 2,
        windowPos.y + (windowSize.y - textSize.y) / 2
    );
    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), message);
}
```

### Alternatives Considered
1. **GetForegroundDrawList()** - Rejected: covers entire screen, not just editor
2. **Separate ImGui window** - Rejected: adds complexity, z-ordering issues
3. **ImGui::BeginDisabled() on entire editor** - Rejected: doesn't provide visual feedback

---

## 3. Input Blocking Strategy

### Decision
Check `ExportState::isExportInProgress()` at the entry point of each input handler in `NodeView` and `ModelEditor`, returning early if export is active.

### Rationale
- Follows existing pattern in `onCreateNode()` and `onDeleteNode()`
- Minimal code changes
- No modification to ImGui event handling
- Thread-safe via atomic read

### Components Requiring Protection
| Component | Method/Area | Current Status | Action Needed |
|-----------|-------------|----------------|---------------|
| ModelEditor | onCreateNode() | ✅ Protected | None |
| ModelEditor | onDeleteNode() | ✅ Protected | None |
| ModelEditor | Keyboard shortcuts (Ctrl+C/V) | ❌ Not protected | Add check |
| ModelEditor | Undo/Redo menu items | ❌ Not protected | Add check |
| NodeView | viewFloat() | ❌ Not protected | Add check |
| NodeView | viewFloat3() | ❌ Not protected | Add check |
| NodeView | viewString() | ❌ Not protected | Add check |
| NodeView | viewInt() | ❌ Not protected | Add check |
| NodeView | viewMatrix() | ❌ Not protected | Add check |
| NodeView | viewResource() | ❌ Not protected | Add check |
| MainWindow | File menu items | ✅ Protected | Verify |

### Implementation Pattern
```cpp
void NodeView::viewFloat(...)
{
    // Add at method entry (or around input widgets)
    if (m_exportState && m_exportState->isExportInProgress())
    {
        ImGui::BeginDisabled();
    }
    // ... existing code ...
    if (m_exportState && m_exportState->isExportInProgress())
    {
        ImGui::EndDisabled();
    }
}
```

### Alternatives Considered
1. **Global input filter** - Rejected: ImGui doesn't support this cleanly
2. **Wrapper widget functions** - Rejected: too invasive, breaks existing code
3. **Per-window disabled flag** - Rejected: ImGui window disabled flag doesn't propagate to node-editor

---

## 4. File Operation Blocking

### Decision
File operations (New, Open, Import) are already blocked via `ImGui::BeginDisabled()` in MainWindow when `exportInProgress || loadingInProgress`.

### Verification
Confirmed in `MainWindow.cpp` lines 1181-1199:
```cpp
bool const exportInProgress = m_exportState.isExportInProgress();
bool const loadingInProgress = m_doc && m_doc->isLoadingInProgress();
bool const operationInProgress = exportInProgress || loadingInProgress;
// ...
ImGui::BeginDisabled(operationInProgress);
if (ImGui::MenuItem("New")) { ... }
if (ImGui::MenuItem("Open")) { ... }
// ...
ImGui::EndDisabled();
```

### Action Needed
None for file operations - already implemented correctly.

---

## 5. Application Close During Export

### Decision
Show a confirmation dialog when user attempts to close the application during an active export.

### Rationale
- Prevents accidental data loss
- Allows user to wait for export completion
- Follows standard application patterns

### Implementation Location
This is typically handled in the main event loop or window close handler. Need to investigate where window close is handled.

### Action Needed
Research window close handling in Application.cpp or MainWindow.cpp.

---

## Summary

| Research Topic | Decision | Confidence |
|----------------|----------|------------|
| State tracking | Use existing ExportState | High |
| Overlay rendering | GetWindowDrawList after ed::End | High |
| Input blocking | Check at handler entry points | High |
| File operations | Already blocked in MainWindow | Verified |
| Application close | Confirmation dialog | Needs implementation |
