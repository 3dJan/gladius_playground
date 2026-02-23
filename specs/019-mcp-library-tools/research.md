# Research: MCP Library Tools

**Phase 0 output** — resolves all NEEDS CLARIFICATION items and documents key design decisions.

## R1: Library File System Layout

**Decision**: Reuse the existing two-tier library directory structure as-is.

**Rationale**: The library system is already well-established with both shipped (`<appDir>/library/`) and user (`~/.local/share/gladius/library/`) directories. Categories are subdirectories. LibraryBrowser already scans this layout. No changes needed to the directory structure.

**Details**:
- User library root: obtained via `platform-folders` (sfl::getCacheFolder or similar), resolves to `~/.local/share/gladius/library/` on Linux
- Categories: subdirectories within library root (e.g., `primitives/`, `lattices/`)
- Entries: `.3mf` files within category directories
- Metadata: model-level 3MF metadata with namespace `"gladius"`, keys `"library-functions"` and `"library-description"`

**Alternatives considered**: Per-entry JSON sidecar files — rejected because the existing 3MF metadata approach is already implemented and integrated.

---

## R2: Library Metadata System

**Decision**: Reuse `gladius::io` free functions from `LibraryMetadata.{h,cpp}`.

**Rationale**: All needed operations exist:
- `readLibraryMetadata(model)` → reads `gladius:library-functions` and `gladius:library-description`
- `writeLibraryMetadata(model, metadata)` → stamps metadata onto a lib3mf model
- `computeSelectiveImportClosure(model, taggedIds, logger)` → computes transitive dependency set
- `pruneModelForSelectiveImport(model, closureIds)` → removes non-closure resources
- `parseResourceIds(string)` / `serializeResourceIds(ids)` → semicolon-separated ID parsing

**Key finding**: `pruneModelForSelectiveImport()` removes build items and non-closure functions but does **not** remove mesh objects (docs say "can be cleaned up separately").

**Alternatives considered**: Building a new metadata system — rejected; existing one is complete and tested.

---

## R3: Export Strategy — Full vs Pruned

**Decision**: Export the full project (same as LibraryExportDialog), tag with metadata, and rely on selective import at import time.

**Rationale**: The existing LibraryExportDialog follows this pattern explicitly. The dialog even shows a UI note: *"The exported file will contain the complete project. Only the selected function will be importable via the library."* The reason is that lib3mf's `RemoveResource` breaks internal state on models with cross-function ResourceIdNode references.

**Details of export flow** (from LibraryExportDialog):
1. `doc->update3mfModel()` — sync internal graph to 3MF
2. Write library metadata to the source model (temporarily)
3. Save full model to target path via `model->QueryWriter("3mf")->WriteToFile(path)`
4. Remove library metadata from the source model (cleanup)

**Alternative considered**: Pruning before export — rejected due to lib3mf `RemoveResource` bug with cross-references. Would require creating a separate model copy, which adds complexity.

**Implication for `export_to_library` tool**: The tool saves the full document with metadata. Import uses `computeSelectiveImportClosure` to import only tagged functions.

---

## R4: Import Strategy — Merging into Active Document

**Decision**: Use lib3mf's model merge with selective import closure.

**Rationale**: lib3mf has `model->MergeFromModel(otherModel)` capability. Combined with `computeSelectiveImportClosure` and `pruneModelForSelectiveImport`, this enables importing only the tagged functions and their dependencies.

**Import flow** (design):
1. Open library 3MF file into a temporary lib3mf model (NOT as the active document)
2. Read metadata to get tagged resource IDs
3. Compute selective import closure
4. Prune the temporary model to only closure resources
5. Merge pruned model into the active document's model
6. Sync assembly from the merged 3MF model
7. Return the new resource IDs

**Key risk**: Resource ID collisions during merge — lib3mf should handle re-numbering, but needs verification.

**Alternative considered**: Open-copy-reopen approach (open library file, copy function graph JSON, reopen original doc, paste) — rejected because it would lose non-graph metadata and is fragile.

---

## R5: Tool Registration Architecture

**Decision**: Register new library tools as lambdas in `setupBuiltinTools()`, following the existing pattern.

**Rationale**: All 31 existing tools are registered this way. The tool implementation classes (`FunctionOperationsTool`, etc.) exist but are called through lambdas, not through a plugin system. The simplest approach is to follow the same pattern.

**Registration pattern**:
```cpp
registerTool("tool_name", "description", schema_json,
    [this](const json& params) -> json { ... });
```

**New interface methods needed on MCPApplicationInterface**:
- `listLibrary(std::optional<std::string> category)` → `nlohmann::json`
- `getLibraryEntryInfo(std::string category, std::string name)` → `nlohmann::json`
- `createLibraryEntry(std::string name, std::string category, std::string expression, std::string description, std::vector<FunctionArgument> arguments)` → `nlohmann::json`
- `exportToLibrary(uint32_t functionId, std::string category, std::string name, std::string description)` → `nlohmann::json`
- `importLibraryEntry(std::string category, std::string name)` → `nlohmann::json`
- `deleteLibraryEntry(std::string category, std::string name)` → `nlohmann::json`

**Alternative considered**: A tool plugin system with registry — rejected per YAGNI; the existing pattern works and is consistent.

---

## R6: Stdio Transport Pollution Sources

**Decision**: Fix three specific stdout pollution issues.

**Findings**:

| Source | Location | Issue | Fix |
|---|---|---|---|
| Tool registration logs | `MCPServer.cpp:142` | `registerTool()` prints `"Registered MCP tool: ..."` when `m_transportType == HTTP`, but transport type defaults to HTTP at construction time, so all 31 registration messages go to stdout before `start(STDIO)` is called | Change to stderr or suppress entirely for non-HTTP modes |
| Server stop message | `MCPServer.cpp:240` | `"MCP Server stopped"` is unconditionally printed to stdout | Guard with transport type check or redirect to stderr |
| Duplicate Application | `main.cpp:~185` | Second `Application app(headless)` construction shadows the outer one | Remove the duplicate line |

**Additional notes**: The `sendStdioResponse()` method correctly uses stdout for JSON-RPC responses — this is intentional and correct.

---

## R7: Error Message Enhancement Pattern

**Decision**: Add a `generateUsageExample()` helper to MCPToolBase or as a utility function, and use it in tool error handlers.

**Rationale**: Some tools already include usage guidance (e.g., `create_function_from_expression` lists supported syntax on parse failure). The goal is to make this systematic across all tools, especially the new library tools.

**Pattern** (design):
```json
{
  "success": false,
  "error": "Missing required parameter: name",
  "required_parameters": [
    {"name": "name", "type": "string", "description": "Function name"},
    {"name": "category", "type": "string", "description": "Library category"}
  ],
  "usage_example": {
    "name": "my-function",
    "category": "primitives",
    "expression": "sqrt(x*x + y*y + z*z) - 5",
    "description": "Sphere with radius 5"
  }
}
```

**Alternative considered**: Returning error codes only — rejected per spec requirement FR-014.

---

## R8: MCP Stdio Framing Protocol

**Decision**: Keep newline-delimited JSON (current implementation), which matches the MCP specification for stdio transport.

**Rationale**: Research confirmed that the MCP specification (2024-11-05) uses newline-delimited JSON for stdio transport, not HTTP Content-Length framing. The current `std::getline` + `json::parse` + `std::cout << response.dump() << std::endl` implementation is correct for MCP stdio. The previous health check found that Content-Length framed input fails — this is **expected and correct behavior** since MCP stdio doesn't use Content-Length framing.

**The actual transport problems** are stdout pollution (R6) and duplicate Application construction, not framing.
