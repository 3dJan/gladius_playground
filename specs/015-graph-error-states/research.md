# Research: Graph Error States

**Feature**: 015-graph-error-states  
**Date**: January 24, 2026

## Research Questions

### 1. How does the current validation system work?

**Decision**: Extend existing `Validator` class and `ValidationError` struct

**Findings**:
- `Validator::validate(Assembly&)` iterates all functions, calls `validateModel()` for each
- `ValidationError` struct contains: `message`, `model`, `node`, `port`, `parameter`
- `Document::validateAssembly()` calls validator, then logs each error as an event
- Validation is called from `refreshModelAsync()` (line 108) and `updateFlatAssembly()` (line 225)
- If validation fails, these methods return early, preventing code generation

**Current flow**:
```
User edits graph
  → Document::refreshModelIfNoCompilationIsRunning()
    → refreshModelAsync()
      → validateAssembly() ← logs events here
        → Validator::validate()
          → validateModel() for each function
            → validateNode() for each node
      → if invalid: return early
      → else: recompileIfRequired(), precomputeSdf...
```

**Rationale**: The existing structure already separates validation from compilation. We need to:
1. Stop `validateAssembly()` from logging events during interactive editing
2. Store issues in a persistent list accessible to UI
3. Add a context parameter to distinguish interactive vs API/file-load

**Alternatives considered**:
- Complete rewrite of validation system → Rejected: Too much risk, existing system works
- Event-based issue notification → Rejected: Complicates UI binding unnecessarily

---

### 2. How should IssueList be structured?

**Decision**: Simple container class with query methods

**Findings**:
- Issues need to be queryable by model ID (for graph editor overlay)
- Issues need to be iterable (for Event Viewer global list)
- Issues need unique identity for comparison (detect resolved vs new)
- Thread safety needed (validation may run on background thread)

**Rationale**: A simple class with mutex-protected operations and model-filtering is sufficient.

**Alternatives considered**:
- Use `std::vector<ValidationError>` directly → Rejected: Need query methods, thread safety
- Observable pattern with callbacks → Rejected: YAGNI, ImGui polls each frame anyway

---

### 3. Where should IssueList be owned?

**Decision**: Owned by `Document`, accessible via getter

**Findings**:
- `Document` already owns `m_assembly` and calls `validateAssembly()`
- `Document` is accessible from both `ModelEditor` and API
- Single source of truth prevents synchronization issues

**Rationale**: Follows existing pattern where Document is the central model container.

**Alternatives considered**:
- Global singleton → Rejected: Violates constitution (no globals)
- Owned by Validator → Rejected: Validator is transient, created per validation call

---

### 4. How to implement ValidationContext (interactive vs API/file-load)?

**Decision**: Add `enum class ValidationContext { Interactive, FileLoad, Api }` parameter

**Findings**:
- `Document::validateAssembly()` is called from multiple places
- Need to distinguish: `refreshModelAsync()` (interactive), `loadImpl()` (file load), API tools
- File load already clears event log, so emitting events once is appropriate
- Interactive editing should populate IssueList only, no events

**Implementation approach**:
```cpp
bool Document::validateAssembly(ValidationContext context) const;
// context == Interactive: populate IssueList, no events
// context == FileLoad/Api: populate IssueList AND emit events once
```

**Rationale**: Minimal change, clear semantics.

**Alternatives considered**:
- Boolean flag `silent` → Rejected: Less expressive, harder to extend
- Separate methods `validateSilent()` / `validateWithEvents()` → Rejected: Code duplication

---

### 5. How to implement collapsible overlay in graph editor?

**Decision**: Use ImGui window with `ImGuiWindowFlags_NoDecoration` anchored to editor

**Findings**:
- `ModelEditor` uses ImGui for all UI
- ImGui child windows can be positioned relative to parent
- `ImGui::BeginChild()` with collapsible header pattern is idiomatic
- Click handlers can use `ImGui::Selectable()` with callbacks

**Implementation approach**:
```cpp
void ModelEditor::renderIssuesOverlay()
{
    auto issues = m_doc->getIssueList().getIssuesForModel(m_currentModel->getResourceId());
    if (issues.empty()) return;
    
    ImGui::SetNextWindowPos(/* top-right of editor */);
    if (ImGui::CollapsingHeader("Issues", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (auto const& issue : issues)
        {
            if (ImGui::Selectable(issue.message.c_str()))
            {
                requestNodeFocus(issue.nodeId);
            }
        }
    }
}
```

**Rationale**: Follows existing ImGui patterns in codebase.

**Alternatives considered**:
- Separate floating window → Rejected: Could obstruct editing
- Status bar only → Rejected: Not enough space for multiple issues

---

### 6. How to implement click-to-navigate?

**Decision**: Use existing `ModelEditor::requestNodeFocus()` mechanism

**Findings**:
- `ModelEditor` already has `m_nodeToFocus` and `m_shouldFocusNode` fields
- `requestNodeFocus(NodeId)` exists for programmatic navigation
- Node editor library supports centering view on node

**Rationale**: Reuse existing infrastructure.

---

### 7. What fix suggestions to provide for each issue type?

**Decision**: Add `IssueType` enum and `fixSuggestion` field to ValidationError

**Issue types and suggestions**:

| Issue Type | Fix Suggestion |
|------------|----------------|
| `MissingConnection` | "Connect an output from another node to this parameter" |
| `TypeMismatch` | "Expected type {expected}, got {actual}. Check node documentation" |
| `InvalidReference` | "The referenced node/port was deleted. Reconnect or remove connection" |
| `CyclicDependency` | "Remove one of the connections forming the cycle: {nodes}" |
| `FunctionNotFound` | "The referenced function no longer exists. Update or delete this node" |

**Rationale**: Each existing validation check maps to a clear issue type with actionable suggestion.

---

### 8. How to debounce validation during rapid editing?

**Decision**: Use existing frame-based update pattern (validation runs at most once per frame)

**Findings**:
- `refreshModelIfNoCompilationIsRunning()` already checks compilation state
- ImGui frame rate (~60fps) provides natural debouncing
- No need for explicit timer-based debouncing

**Rationale**: Existing architecture already prevents excessive validation.

**Alternatives considered**:
- Explicit debounce timer → Rejected: YAGNI, existing pattern sufficient

---

## Summary of Decisions

| Question | Decision |
|----------|----------|
| Validation system | Extend existing Validator, add IssueList |
| IssueList structure | Simple container with mutex, model queries |
| IssueList ownership | Owned by Document |
| Interactive vs API | ValidationContext enum parameter |
| Overlay UI | ImGui collapsible child window |
| Click-to-navigate | Reuse requestNodeFocus() |
| Fix suggestions | IssueType enum + fixSuggestion field |
| Debouncing | Use existing frame-based pattern |

All NEEDS CLARIFICATION items have been resolved.
