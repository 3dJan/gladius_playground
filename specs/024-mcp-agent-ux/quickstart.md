# Quickstart: 024-mcp-agent-ux

## Branch

```bash
git checkout 024-mcp-agent-ux
```

## Build

Use the VS Code task **"Build ALL (linux-releaseWithDebug)"** — never invoke cmake or ninja directly.

## Run Tests

Use the VS Code task **"Run Gladius Tests (linux-releaseWithDebug)"** or **"Run Unit Tests (Fast)"**.

---

## Key New Tools

### evaluate_function

Evaluate a function at specific 3D points and read back float values. No rendering required.

**Example — verify a sphere SDF:**
```json
{
  "name": "evaluate_function",
  "arguments": {
    "function_id": 5,
    "samples": [
      {"pos": [0.0, 0.0, 0.0]},
      {"pos": [1.0, 0.0, 0.0]},
      {"pos": [2.0, 0.0, 0.0]}
    ]
  }
}
```
Expected (`length(pos) - 1`): `{ "success": true, "results": [-1.0, 0.0, 1.0] }`

### get_changes_since

Ask what changed in the document since a timestamp. Useful when collaborating with a human in UI mode.

**Workflow:**
1. Record `start_time` = current UTC time.
2. Wait or perform other operations.
3. Call `get_changes_since` with `"since": start_time`.
4. Inspect the `changes` list — each entry gives `resource_type`, `resource_id`, `display_name`, and `type`.
5. For modified functions, call `get_function_snippet` to see the new code before overwriting.

### list_library with keyword filter

```json
{
  "name": "list_library",
  "arguments": { "query": "sphere" }
}
```
Returns only entries whose name, description, or tags contain "sphere" (case-insensitive).

### Enriched get_3mf_structure

`get_3mf_structure` now includes per-function `arguments`, `output_type`, and `snippet_preview` — enough to plan edits without a separate `get_function_snippet` call:

```json
{
  "id": 5,
  "kind": "function",
  "display_name": "sphere",
  "arguments": [{"name": "pos", "type": "vec3"}],
  "output_type": "float",
  "snippet_preview": "float r = length(pos);\nreturn r - radius;"
}
```

---

## Implementation Guide for Developers

### New files to create

| File | Purpose |
|------|---------|
| `gladius/src/mcp/tools/FunctionEvaluatorTool.h` | Declaration of `FunctionEvaluatorTool` and `SnippetEvaluator` |
| `gladius/src/mcp/tools/FunctionEvaluatorTool.cpp` | CPU-based snippet evaluation (parse assignments, evaluate expressions) |
| `gladius/tests/unittests/FunctionEvaluator_tests.cpp` | Unit tests for `SnippetEvaluator` |
| `gladius/tests/unittests/LibraryMetadataTags_tests.cpp` | Unit tests for tags read/write |
| `gladius/tests/apitests/MCP_EvaluateFunction_tests.cpp` | API integration tests for `evaluate_function` |
| `gladius/tests/apitests/MCP_ChangeLog_tests.cpp` | API integration tests for `get_changes_since` |

### Files to modify

| File | Change |
|------|--------|
| `gladius/src/io/3mf/LibraryMetadata.h` | Add `libraryTags` field + `LIBRARY_TAGS_KEY` constant |
| `gladius/src/io/3mf/LibraryMetadata.cpp` | Read/write `libraryTags` in `readLibraryMetadata` / `writeLibraryMetadata` |
| `gladius/src/mcp/MCPApplicationInterface.h` | Add `evaluateFunction()` and `getChangesSince()` virtual methods |
| `gladius/src/mcp/ApplicationMCPAdapter.h` | Add `m_changeLog`, `recordChange()`, `getChangesSince()`, `ChangeEntry` struct |
| `gladius/src/mcp/ApplicationMCPAdapter.cpp` | Add change tracking calls; enrich `get3MFStructure()`; add `listLibrary(category, query)` |
| `gladius/src/mcp/MCPServer.cpp` | Register `evaluate_function`, `get_changes_since`; extend `list_library` schema |
| `gladius/src/mcp/tools/LibraryTool.h` | Add `query` parameter to `listLibrary()`; add `tags` to `setLibraryMetadata()` |
| `gladius/src/mcp/tools/LibraryTool.cpp` | Implement keyword filter; add tags/snippet to `getLibraryEntryInfo()` |
| `.github/skills/creating-library-items/SKILL.md` | Document `evaluate_function`, `get_changes_since`, keyword search |

### Change tracking wiring

In `ApplicationMCPAdapter.cpp`, after each successful write operation, call:
```cpp
recordChange("modified", "function", functionId, displayName);
```

The `m_changeLog` is a `std::deque<ChangeEntry>` (max 1000 entries, oldest dropped).
`getChangesSince` filters by `entry.timestamp > parsedSince` and serialises to JSON.
