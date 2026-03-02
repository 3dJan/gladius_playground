# Quickstart: Graph Error States

**Feature**: 015-graph-error-states

## What This Feature Does

When you edit a function graph and introduce validation errors, the system now:

1. **Collects all issues** in a persistent list instead of logging them repeatedly
2. **Stops update attempts** (no endless error loops)
3. **Shows issues** in two places:
   - Event Viewer (all issues across all models)
   - Collapsible overlay in graph editor (issues for current model only)
4. **Enables click-to-navigate** from issue to affected node
5. **Provides fix suggestions** for each issue type

## User Workflow

### Viewing Issues

1. **Event Viewer Panel**: Shows all validation issues globally
   - Filter by model or issue type
   - Click any issue to navigate to the affected node

2. **Graph Editor Overlay**: Shows issues for the currently open model
   - Appears automatically when issues exist
   - Collapsible to minimize distraction
   - Click any issue to center view on affected node

### Fixing Issues

Each issue includes a fix suggestion. Common scenarios:

| Issue | What It Means | How to Fix |
|-------|---------------|------------|
| "Missing connection" | A required parameter has no input | Connect an output from another node |
| "Type mismatch" | Wrong data type connected | Use a node with compatible output type |
| "Invalid reference" | Connected node/port was deleted | Reconnect or remove the broken connection |
| "Cycle detected" | Graph has circular dependency | Remove one connection in the cycle |
| "Function not found" | Referenced function deleted | Delete node or update reference |

### When Updates Resume

- Updates (code generation, preview rendering) are **blocked** while errors exist
- Once all errors are resolved, updates **resume automatically**
- No manual action required

## For API Users

When using Gladius via API or loading files:

- Validation issues are logged as events **once** (not repeatedly)
- Check `Document::getIssueList().hasErrors()` before proceeding
- Access detailed issues via `Document::getIssueList().getAll()`

```cpp
// Example: Loading a file via API
document.load("model.3mf");

if (document.getIssueList().hasErrors())
{
    for (auto const& issue : document.getIssueList().getAll())
    {
        std::cerr << issue.message << ": " << issue.fixSuggestion << "\n";
    }
    return; // Don't proceed with invalid model
}

// Model is valid, proceed with operations
document.exportAsStl("output.stl");
```
