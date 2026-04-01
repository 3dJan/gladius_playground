# Implementation Plan: MCP Agent UX Improvements

**Branch**: `024-mcp-agent-ux` | **Date**: 2026-03-17 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `/specs/024-mcp-agent-ux/spec.md`

## Summary

The Gladius MCP server exposes implicit-modeling tools to AI agents via the Model Context Protocol. Multiple rounds of tool additions have left gaps that force agents into expensive workarounds: `evaluate_function` does not exist (agents must render to verify SDF values), `get_3mf_structure` omits snippet previews and argument formats, `list_library` has no keyword search, change notifications are absent (agents and humans can silently overwrite each other), and the `creating-library-items` skill is out of date.

This feature closes those gaps with five targeted additions:
1. A new `evaluate_function` tool (OpenCL-based point sampling reusing the existing `ToOCLVisitor` / `ComputeContext` infrastructure to compile and dispatch a 1D evaluation kernel over sample points).
2. Enriching `get_3mf_structure` to include `arguments`, `output_type`, `snippet_preview`, and constant-node parameter values for each function resource.
3. Adding a keyword `query` filter to `list_library`, adding `snippet` to `get_library_entry_info`, adding `tags` to library metadata storage/retrieval.
4. A new `get_changes_since` tool backed by a lightweight in-memory change log in `ApplicationMCPAdapter`.
5. Updating the `creating-library-items` and related skills to reflect all new tools.

`get_function_snippet` already returns `arguments`, `output_type`, and `outputs` — US1 (snippet round-trip) is substantively complete; only a small test-coverage gap needs closing. `create_library_entry` already accepts `snippet` — US5 is done.

---

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: lib3mf (3MF I/O), nlohmann/json (MCP protocol), OpenCL 1.2+ (GPU kernels), ImGui (UI), GTest/GMock (testing)  
**Storage**: Filesystem only — library entries are `.3mf` files; change log is in-memory (per-session, resets on document close)  
**Testing**: GTest/GMock — unit tests in `gladius/tests/unittests/`, API/integration tests in `gladius/tests/apitests/`  
**Target Platform**: Linux (primary), macOS/Windows secondary  
**Project Type**: Single C++ project  
**Performance Goals**: `evaluate_function` must return results for ≤1000 points before MCP timeout (~30 s); `get_3mf_structure` enrichment must add <100 ms overhead over baseline  
**Constraints**: No new external dependencies; change log kept in-memory only (no persistence across sessions); `evaluate_function` uses the existing OpenCL compute path (an OpenCL device — GPU or CPU — must be available)  
**Scale/Scope**: ~6 new/modified source files; ~3 new tool registrations; ~200–400 LOC net new  

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I – Modern C++20 | ✅ PASS | All new code uses smart pointers, `std::chrono`, `std::vector`, no raw owning pointers |
| II – Test-First | ✅ PASS | Each new tool gets unit + API tests; evaluate_function gets an OpenCL integration test |
| III – Simplicity (KISS/DRY/YAGNI) | ✅ PASS | Change log is a `std::deque<ChangeEntry>` — no persistence, no external broker |
| IV – Code Style | ✅ PASS | Allman braces, 4-space indent, east-side const, camelCase, m_ prefix |
| V – Documentation | ✅ PASS | Doxygen on all new public APIs |
| VI – UI Responsiveness | ✅ PASS | `evaluate_function` and `get_changes_since` execute in MCP worker thread, do not touch UI thread |

No violations. No Complexity Tracking section needed.

---

## Research (Phase 0)

### R1 – evaluate_function: evaluation approach

**Decision**: OpenCL-based evaluation reusing the existing GPU compute infrastructure.

**Rationale**: Functions may call other functions and reference shared resources (images, lookup tables). The OpenCL path via `ToOCLVisitor` / `ComputeContext` already handles these dependency graphs correctly — it is the same pipeline used for rendering and meshing. A CPU-side snippet interpreter would need to re-implement all of this cross-function resolution, making it fragile and incomplete. Reusing the OpenCL path avoids that duplication entirely.

The tool compiles the function to an OpenCL kernel via `ToOCLVisitor`, uploads the sample points as a 1D buffer, dispatches the kernel, and reads back results. For ≤1000 points the dispatch overhead is negligible. Both `float` and `vec3` output types are supported.

**Alternatives considered (rejected)**:
- *CPU SnippetEvaluator*: Parse the snippet text into a variable map and evaluate line-by-line using `ExpressionParser`. Fully CPU-based (no GPU dependency), but cannot resolve cross-function calls, resource references, or Image3D lookups — only works for trivial single-function snippets. Rejected because the spec mandates correct evaluation of arbitrary functions.
- *Graph node traversal*: Walk the `nodes::Model` nodes directly. Accurate but tightly coupled to internal node types — brittle and high maintenance cost.

**Headless mode**: An OpenCL device (GPU or CPU driver) must be available. This is already a requirement for the Gladius compute pipeline. In CI/headless environments, an OpenCL CPU driver (e.g., PoCL, Intel CPU Runtime) satisfies the requirement.

### R2 – get_changes_since: change tracking approach

**Decision**: Lightweight in-memory ring buffer in `ApplicationMCPAdapter`.

**Rationale**: The spec requires only resource-level granularity (type + ID + name, no parameter diffs). An `std::deque<ChangeEntry>` capped at 1000 entries satisfies the requirement with zero persistence complexity. Timestamps use `std::chrono::system_clock::time_point` serialised as ISO-8601 strings.

**Change sources**:
- `ApplicationMCPAdapter` intercepts all write paths that modify the document: explicit agent tool calls (`setFunctionSnippet`, `setProgramSnippet`, `setParameter`, etc.) and UI-driven changes via `Document` callbacks.
- For UI-driven changes, the `Document` class must fire an observable notification. Gladius currently has an `EventLogger` mechanism; a thin observer callback installed by the adapter is the least-invasive hook.

**Alternatives considered**:
- *SQLite event store*: Persistent, queryable, but overkill and adds a dependency.
- *File-based log*: Persistent across restarts, but spec says no diff data, so persistence adds no value.

### R3 – Library tags: storage format

**Decision**: Add a third `libraryTags` field to `LibraryMetadata`, stored as a new `gladius:library-tags` metadata key in the 3MF file, comma-separated.

**Rationale**: Mirrors the existing `libraryFunctions` / `libraryDescription` pattern. No new lib3mf API required. Backwards-compatible: missing key yields empty tag list.

### R4 – list_library query filter: implementation

**Decision**: Case-insensitive substring match across entry name and description in `LibraryTool::listLibrary`. Tags field also searched once available.

**Rationale**: Simple, stateless, no index required for the expected library size (<200 entries). Delegated to `LibraryTool::listLibrary` by passing the query string through `ApplicationMCPAdapter`.

---

## Data Model (Phase 1)

### Change Log Entry

```cpp
/// Stored in ApplicationMCPAdapter::m_changeLog
struct ChangeEntry
{
    std::chrono::system_clock::time_point timestamp;
    std::string type;         // "added" | "modified" | "deleted"
    std::string resourceType; // "function" | "levelset" | "parameter" | "document"
    uint32_t resourceId;      // 0 if not applicable (e.g. document-level)
    std::string displayName;  // User-visible name of the affected resource
};
```

### Library Metadata (extended)

```cpp
struct LibraryMetadata
{
    std::string libraryFunctions;   ///< Semicolon-separated resource IDs
    std::string libraryDescription; ///< Free-text description
    std::string libraryTags;        ///< Comma-separated tags (NEW)
};
```

### evaluate_function request/response (JSON contract)

**Request**:
```json
{
  "function_id": 5,
  "samples": [
    {"pos": [0.0, 0.0, 0.0]},
    {"pos": [1.0, 0.0, 0.0]}
  ]
}
```

**Response (success)**:
```json
{
  "success": true,
  "function_id": 5,
  "results": [-1.0, 0.0]
}
```

**Response (error)**:
```json
{
  "success": false,
  "error": "Function contains unsupported nodes; cannot evaluate on CPU",
  "function_id": 5
}
```

### get_changes_since request/response (JSON contract)

**Request**:
```json
{ "since": "2026-03-17T10:00:00Z" }
```

**Response**:
```json
{
  "success": true,
  "changes": [
    {
      "timestamp": "2026-03-17T10:05:32Z",
      "type": "modified",
      "resource_type": "function",
      "resource_id": 5,
      "display_name": "sphere"
    }
  ]
}
```

### get_3mf_structure function resource (enriched)

Each function resource in the `resources` array gains:
```json
{
  "id": 5,
  "kind": "function",
  "function_type": "implicit",
  "display_name": "sphere",
  "arguments": [{"name": "pos", "type": "vec3"}],
  "output_type": "float",
  "snippet_preview": "float r = length(pos);\nreturn r - radius;"
}
```

---

## API Contracts (Phase 1)

Contracts are expressed as MCP tool schemas. Full schemas are in [contracts/](contracts/).

| Tool | Change | Priority |
|------|--------|----------|
| `evaluate_function` | NEW — evaluate function at N 3D sample points | P1 |
| `get_3mf_structure` | ENRICH — add `arguments`, `output_type`, `snippet_preview` per function | P1 |
| `get_changes_since` | NEW — return change log since ISO-8601 timestamp | P2 |
| `list_library` | EXTEND — add optional `query` keyword filter | P2 |
| `get_library_entry_info` | EXTEND — add `snippet` and `tags` fields to response | P2 |
| `set_library_metadata` | EXTEND — accept and persist `tags` array | P2 |

---

## Project Structure

### Documentation (this feature)

```text
specs/024-mcp-agent-ux/
├── plan.md              # This file
├── research.md          # Phase 0 — embedded above (R1–R4 sections)
├── data-model.md        # Phase 1 — embedded above (Data Model section)
├── quickstart.md        # Phase 1 output
├── contracts/
│   ├── evaluate_function.json
│   ├── get_changes_since.json
│   ├── list_library_extended.json
│   └── get_3mf_structure_enriched.json
└── tasks.md             # Phase 2 output (/speckit.tasks — NOT created here)
```

### Source Code (gladius/ project root)

```text
gladius/src/
├── io/3mf/
│   ├── LibraryMetadata.h          MODIFY — add libraryTags field + key constant
│   └── LibraryMetadata.cpp        MODIFY — read/write libraryTags
├── mcp/
│   ├── ApplicationMCPAdapter.h    MODIFY — add m_changeLog + recordChange() + getChangesSince()
│   ├── ApplicationMCPAdapter.cpp  MODIFY — instrument all write-path methods; add listLibrary(query)
│   ├── MCPApplicationInterface.h  MODIFY — add evaluateFunction(), getChangesSince() to interface
│   ├── MCPServer.cpp              MODIFY — register evaluate_function, get_changes_since, extend
│   │                                       list_library schema, extend set_library_metadata schema
│   └── tools/
│       ├── FunctionEvaluatorTool.h    NEW — evaluate_function implementation
│       ├── FunctionEvaluatorTool.cpp  NEW — OpenCL-based function evaluation
│       └── LibraryTool.cpp        MODIFY — add query filter to listLibrary(); add tags/snippet to
│                                           getLibraryEntryInfo(); add tags to setLibraryMetadata()

gladius/tests/
├── unittests/
│   ├── FunctionEvaluator_tests.cpp    NEW — unit tests for FunctionEvaluatorTool
│   └── LibraryMetadataTags_tests.cpp  NEW — unit tests for tag read/write
└── apitests/
    ├── MCP_EvaluateFunction_tests.cpp  NEW — API tests for evaluate_function
    └── MCP_ChangeLog_tests.cpp         NEW — API tests for get_changes_since

.github/skills/creating-library-items/SKILL.md  MODIFY — reflect new tools
```

**Structure Decision**: Single C++ project — all changes within `gladius/`. No new top-level directories. New tool lives in the existing `src/mcp/tools/` pattern. Test files follow the existing pattern (`*_tests.cpp` in `unittests/` or `apitests/`).

---

## Implementation Notes

### evaluate_function (US2)

`FunctionEvaluatorTool` is a new `MCPToolBase` subclass. Its `evaluateFunction(uint32_t functionId, const std::vector<SamplePoint>&)` method:
1. Looks up the function resource by `functionId` in the current document.
2. Compiles the function's node graph to an OpenCL kernel via `ToOCLVisitor`, reusing the existing `ComputeContext`.
3. Uploads the sample points as a 1D input buffer.
4. Dispatches the kernel with `global_work_size = number_of_samples`.
5. Reads back results — `float` values for scalar outputs, `vec3` (3 floats) for vector outputs.
6. If a sample point causes a runtime fault (e.g., division by zero, NaN), the result for that point is `null` and a top-level `warnings` array lists the affected sample indices.
7. If compilation fails (e.g., unsupported node types), returns `success: false` with a descriptive error.

Both `float` and `vec3` output types are supported (spec FR-007, Assumptions). The tool runs in the MCP worker thread and does not block the UI.

`ApplicationMCPAdapter` gets a thin `evaluateFunction` delegation method; `MCPApplicationInterface` gains the virtual method.

### get_3mf_structure enrichment (US3)

Inside `ApplicationMCPAdapter::get3MFStructure()`, for each function resource that is an `CImplicitFunction`:
- Call `getFunctionSnippet(resourceId)` and take the first 3 lines as `snippet_preview`.
- Reformat the existing `inputs` array into `arguments` (normalise `scalar`→`float`, `vector`→`vec3`).
- Set `output_type` by inspecting the first connected output port.

Snippet retrieval adds per-function graph traversal. This is bounded by the number of functions (typically <30) and does not block the UI thread (MCP tools run in worker threads).

### get_changes_since (US6)

`ApplicationMCPAdapter` gains:
```cpp
struct ChangeEntry { ... };
std::deque<ChangeEntry> m_changeLog;  // bounded to 1000 entries
void recordChange(std::string type, std::string resourceType, uint32_t id, std::string name);
nlohmann::json getChangesSince(std::string const& isoTimestamp) const;
```

All write methods (`setFunctionSnippet`, `setProgramSnippet`, `setParameter`, `createFunctionFromSnippet`, `createFunctionFromExpression`, `createLevelSet`, `openDocument`, `createNewDocument`) call `recordChange` after success.

For UI-driven changes: install a `Document::setOnModifiedCallback` (or equivalent observable) in the adapter's constructor to call `recordChange("modified", "document", 0, "")`. The change log at this level is resource-level only — agents inspect specific functions with `get_function_snippet` after detecting changes.

### Library tags (US4)

`LibraryMetadata.h` gains:
```cpp
inline auto constexpr LIBRARY_TAGS_KEY = "library-tags";

struct LibraryMetadata {
    std::string libraryFunctions;
    std::string libraryDescription;
    std::string libraryTags;   // comma-separated, may be empty
};
```

`readLibraryMetadata` / `writeLibraryMetadata` updated to handle the new key. `LibraryTool::getLibraryEntryInfo` returns a `tags` array decoded from the comma-separated string. `LibraryTool::setLibraryMetadata` accepts an optional `tags` vector and encodes/writes it.

### list_library keyword filter (US4)

`LibraryTool::listLibrary(std::string const& category, std::string const& query)` gains a second parameter. If `query` is non-empty, entries are filtered to those where `name` or `description` contains the query string (case-insensitive via `std::tolower`). Tags are also searched once populated.

`ApplicationMCPAdapter::listLibrary(std::string const& category, std::string const& query)` passes through. `MCPServer.cpp` updates the `list_library` tool schema to add an optional `query` string parameter.

---

## Quickstart (Phase 1)

### Building

```bash
# Use VS Code task — never cmake directly
# Task: "Build ALL (linux-releaseWithDebug)"
```

### Running unit tests

```bash
# Task: "Run Unit Tests (Fast)" or "Run Unit Tests (CTest)"
```

### Testing evaluate_function manually (via MCP)

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

Expected response for a unit sphere (`length(pos) - 1`):
```json
{ "success": true, "results": [-1.0, 0.0, 1.0] }
```

### Testing get_changes_since manually

1. Save the current ISO-8601 time.
2. Call `set_function_snippet` to modify a function.
3. Call `get_changes_since` with the saved timestamp — expect one "modified" entry.

---

## Constitution Check (Post-Design)

| Principle | Status | Notes |
|-----------|--------|-------|
| I – Modern C++20 | ✅ PASS | `std::deque`, `std::chrono`, `std::optional`, smart pointers throughout |
| II – Test-First | ✅ PASS | New unit tests for `SnippetEvaluator` and `LibraryMetadataTags`; API tests for all new tools |
| III – Simplicity | ✅ PASS | CPU evaluator, in-memory change log, substring filter — no over-engineering |
| IV – Code Style | ✅ PASS | All new files follow existing patterns in `src/mcp/tools/` |
| V – Documentation | ✅ PASS | Doxygen on `FunctionEvaluatorTool`, `ChangeEntry`, extended `LibraryMetadata` |
| VI – UI Responsiveness | ✅ PASS | All MCP tools run in worker thread; change log access protected by existing adapter mutex patterns |

Complexity Tracking: No constitution violations — no justification section needed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| [e.g., 4th project] | [current need] | [why 3 projects insufficient] |
| [e.g., Repository pattern] | [specific problem] | [why direct DB access insufficient] |
