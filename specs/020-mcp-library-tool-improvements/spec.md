# Spec 020: MCP Library Tool Improvements

**Goal**: Make library creation via MCP tools a reliable, single-session workflow
that an AI agent can complete without workarounds.

**Context**: During hands-on testing of the library creation skill
(`docs/skills/creating_library_items.md`), several tool bugs and missing
capabilities were identified. This spec proposes fixes ordered by impact.

---

## 1. Fix `set_parameter` numeric string coercion (Bug)

**File**: [MCPServer.cpp](../../gladius/src/mcp/MCPServer.cpp) ~line 1210

**Problem**: When `type="string"` and `value="19"`, JSON parsing converts the
value to an integer before the handler sees it. The line
`std::string string_value = value;` then throws
`[json.exception.type_error.302] type must be string, but is number`.

This blocks setting `gladius:library-functions` metadata (which is always a
numeric string like `"5"` or `"5;12"`).

**Root cause**: The JSON schema for `value` has no type constraint:
```cpp
{"value", {{"description", "Parameter value (number or string)"}}}
```
MCP clients are free to send `"value": "19"`, but the JSON transport may
parse it as integer `19` if the schema doesn't force string.

**Fix**: In the `type == "string"` branch, coerce any JSON type to string:

```cpp
else if (type == "string")
{
    std::string string_value;
    if (value.is_string())
    {
        string_value = value.get<std::string>();
    }
    else if (value.is_number_integer())
    {
        string_value = std::to_string(value.get<long long>());
    }
    else if (value.is_number_float())
    {
        string_value = std::to_string(value.get<double>());
    }
    else
    {
        string_value = value.dump();  // fallback: serialize to JSON string
    }
    success = m_application->setStringParameter(
        model_id, node_name, parameter_name, string_value);
}
```

**Effort**: ~10 lines changed in one file. No new APIs.

**Tests**: Add a unit test that calls `set_parameter` with
`type="string", value=19` (integer) and verifies it succeeds.

---

## 2. `export_to_library`: add `keep_scaffold` option

**File**: [LibraryTool.cpp](../../gladius/src/mcp/tools/LibraryTool.cpp),
[MCPServer.cpp](../../gladius/src/mcp/MCPServer.cpp) ~line 1780

**Problem**: `export_to_library` always prunes the file, removing the build item,
levelset, mesh, and `main` function. This produces a minimal file that cannot
render standalone and doesn't match the shipped library entries (which all have
the demo scaffold).

Currently, creating a proper library entry requires a manual 8-step workflow
using `save_document_as` + Python ZIP patching for metadata.

**Proposed change**: Add an optional boolean parameter `keep_scaffold` (default `false`
for backward compatibility). When `true`:

1. Skip the call to `pruneExportedLibraryFile()`.
2. Still stamp `gladius:library-functions` and `gladius:library-description`
   metadata.
3. Still embed the thumbnail.

This makes `export_to_library(function_id=19, category="primitives",
name="cylinder", description="...", keep_scaffold=true)` a single-call
replacement for the entire 8-step workflow.

**Implementation sketch** (LibraryTool.cpp, `exportToLibrary()`):

```cpp
// Add parameter
nlohmann::json LibraryTool::exportToLibrary(uint32_t functionId,
                                             std::string const & category,
                                             std::string const & name,
                                             std::string const & description,
                                             bool overwrite,
                                             bool keepScaffold)  // NEW
{
    // ...existing code up to writing the file...

    // Conditionally prune
    std::size_t prunedCount = 0;
    if (!keepScaffold)
    {
        prunedCount = pruneExportedLibraryFile(
            targetPath, static_cast<Lib3MF_uint32>(functionId),
            thumbnailPng, logger);
    }
    else if (!thumbnailPng.empty())
    {
        // Still embed thumbnail without pruning
        embedThumbnail(targetPath, thumbnailPng);
    }
    // ...rest unchanged...
}
```

MCPServer.cpp registration — add `keep_scaffold` to the schema:
```cpp
{"keep_scaffold",
 {{"type", "boolean"},
  {"description",
   "If true, keep the full document scaffold (build items, levelset, mesh, "
   "main function) so the entry renders standalone. Default: false."},
  {"default", false}}}
```

**Effort**: ~30 lines across 2 files. Need to extract thumbnail embedding into
a small helper since it's currently inside `pruneExportedLibraryFile`.

**Tests**: Export with `keep_scaffold=true`, then `get_library_entry_info` and
verify `is_tagged: true`, description present, and file contains build items.

---

## 3. Auto-set `resourceid` in `create_function_call_node`

**File**: [FunctionOperationsTool.cpp](../../gladius/src/mcp/tools/FunctionOperationsTool.cpp)
~line 1548

**Problem**: `create_function_call_node` receives `referenced_function_id` and
creates a Resource node, but does **not** set the `resourceid` parameter on it.
The caller must always follow up with `set_parameter_value(node_id=...,
parameter_name="resourceid", value=...)`. Forgetting this (easy to do) causes
cryptic "undeclared identifier" errors during validation.

**Current code** (already sets the parameter internally!):
```cpp
resourceNode->parameter().at(nodes::FieldNames::ResourceId) =
    nodes::VariantParameter(referencedFunctionId);
```

This sets it in the C++ parameter map — but the question is whether this
survives the graph update and 3MF sync. Let me verify...

**Finding**: The code **already sets** the resourceid via direct parameter
assignment. But earlier testing showed that `set_parameter_value` was still
needed after `create_function_call_node`. This suggests the direct assignment
might not persist through `update3mfModel()` or the parameter might be
overwritten during `updateGraphAndOrderIfNeeded()`.

**Investigation needed**: Add a test that:
1. Calls `create_function_call_node`
2. Immediately calls `validate_model(compile=true)`
3. Checks if it succeeds without any intermediate `set_parameter_value`

If it fails, the root cause is likely that the VariantParameter assignment
is being reset during graph update. The fix would be to ensure the resourceid
persists, or to call `model->invalidateGraph()` after setting it.

**Effort**: Investigation + potential 5-line fix. Medium confidence.

---

## 4. Expose `create_constant_nodes_for_missing_parameters` as MCP tool

**File**: [MCPServer.cpp](../../gladius/src/mcp/MCPServer.cpp),
[FunctionOperationsTool.cpp](../../gladius/src/mcp/tools/FunctionOperationsTool.cpp)

**Problem**: `FunctionOperationsTool::createConstantNodesForMissingParameters()`
already exists (~line 1657) and does exactly what agents need: finds all
unconnected required parameters on a node and creates appropriate constant
nodes (ConstantScalar, ConstantVector, etc.) with optional auto-connect.

But it's **not registered as an MCP tool** — it's only used internally.

**Proposed change**: Register it in MCPServer.cpp:

```cpp
registerTool(
    "create_constant_nodes_for_missing_parameters",
    "Create constant nodes for all unconnected required parameters on a node. "
    "Optionally auto-connects them. Returns created nodes and links.",
    {{"type", "object"},
     {"properties",
      {{"function_id", {{"type", "integer"}, {"description", "Function ID"}}},
       {"node_id", {{"type", "integer"}, {"description", "Node to create constants for"}}},
       {"auto_connect",
        {{"type", "boolean"},
         {"description", "Auto-connect created constants to the node (default: true)"},
         {"default", true}}}}},
     {"required", {"function_id", "node_id"}}},
    [this](const json & params) -> json {
        uint32_t functionId = params["function_id"];
        uint32_t nodeId = params["node_id"];
        bool autoConnect = params.value("auto_connect", true);
        return m_application->createConstantNodesForMissingParameters(
            functionId, nodeId, autoConnect);
    });
```

This would replace the tedious 3-step pattern (create_node → set_parameter_value
→ create_link) with a single call after `create_function_call_node`.

**Also needed**: Add the delegation method to `MCPApplicationInterface` and
`ApplicationMCPAdapter` (following the existing pattern).

**Effort**: ~40 lines across 3 files (registration + delegation wiring). The
business logic already exists and is tested.

---

## 5. Improve validation error diagnostics

**File**: [ValidationTool.cpp](../../gladius/src/mcp/tools/ValidationTool.cpp)

**Problem**: When validation fails due to a missing `resourceid`, the error
message is the raw OpenCL compiler output: `use of undeclared identifier
'gladius_Implicit_...'`. This doesn't hint at the actual cause.

**Proposed change**: In `ValidationTool::validateModel()`, after the OpenCL
compile phase, scan the error messages for the `undeclared identifier` pattern
and add a diagnostic hint:

```cpp
// After phase 2 completes with errors:
if (!compileOk)
{
    // Check for the common "undeclared identifier" pattern
    for (auto const & msg : phase2["messages"])
    {
        if (msg["message"].get<std::string>().find("undeclared identifier") != std::string::npos)
        {
            auto hint = nlohmann::json{
                {"severity", "info"},
                {"message", "Hint: 'undeclared identifier' errors usually mean a "
                            "Resource node is missing its 'resourceid' parameter. "
                            "After create_function_call_node, set the resourceid on "
                            "the Resource node using set_parameter_value."}};
            phase2["messages"].push_back(hint);
            break;
        }
    }
}
```

**Effort**: ~15 lines in one file.

---

## 6. Add `set_library_metadata` dedicated tool

**Files**: [MCPServer.cpp](../../gladius/src/mcp/MCPServer.cpp),
[LibraryTool.h/cpp](../../gladius/src/mcp/tools/LibraryTool.h)

**Problem**: Setting library metadata currently requires the generic
`set_parameter` tool (which has the numeric string bug) or Python workarounds.
A dedicated tool would bypass JSON type coercion entirely.

**Proposed tool**:

```cpp
registerTool(
    "set_library_metadata",
    "Set library metadata (tagged functions and description) on the current document. "
    "This stamps the gladius:library-functions and gladius:library-description metadata.",
    {{"type", "object"},
     {"properties",
      {{"function_ids",
        {{"type", "array"},
         {"items", {{"type", "integer"}}},
         {"description", "Resource IDs of tagged (importable) functions"}}},
       {"description",
        {{"type", "string"},
         {"description", "Human-readable description of the library entry"}}}}},
     {"required", {"function_ids", "description"}}},
    [this](const json & params) -> json {
        auto functionIds = params["function_ids"].get<std::vector<uint32_t>>();
        auto description = params["description"].get<std::string>();
        return m_application->setLibraryMetadata(functionIds, description);
    });
```

Implementation in LibraryTool:
```cpp
nlohmann::json LibraryTool::setLibraryMetadata(
    std::vector<uint32_t> const & functionIds,
    std::string const & description)
{
    if (!validateActiveDocument())
        return createToolError("No active document");

    auto document = m_application->getCurrentDocument();
    auto model3mf = document->get3mfModel();

    io::LibraryMetadata metadata;
    std::vector<Lib3MF_uint32> ids(functionIds.begin(), functionIds.end());
    metadata.libraryFunctions = io::serializeResourceIds(ids);
    metadata.libraryDescription = description;
    io::writeLibraryMetadata(model3mf, metadata);

    return {{"success", true},
            {"function_ids", functionIds},
            {"description", description},
            {"message", "Library metadata set successfully"}};
}
```

**Effort**: ~50 lines across 4 files.

**Note**: If fix #1 (numeric string coercion) lands, this tool becomes a
nice-to-have rather than essential. But it's still better UX since it accepts
integer IDs directly and handles serialization internally.

---

## 7. `save_document_as` should persist in-memory metadata

**File**: [DocumentLifecycleTool.cpp](../../gladius/src/mcp/tools/DocumentLifecycleTool.cpp)
or [Document.cpp](../../gladius/src/Document.cpp)

**Problem**: Metadata set via `set_parameter` is applied to the lib3mf model
in memory. But `save_document_as` calls `document->update3mfModel()` which may
reset metadata. The result is that metadata set before saving doesn't appear
in the output file.

**Investigation needed**: Trace the `update3mfModel()` → `writeLibraryMetadata`
path to determine if metadata survives the round-trip. If not, `save_document_as`
should preserve existing metadata by reading it before update and re-stamping
after.

**Effort**: Investigation + potential 10-line fix.

---

## Priority Order

| # | Item | Impact | Effort | Priority |
|---|------|--------|--------|----------|
| 1 | Fix `set_parameter` numeric string | Critical bug | Tiny | **P0** |
| 2 | `export_to_library` keep_scaffold | Eliminates 8-step workaround | Small | **P0** |
| 3 | Auto-set resourceid investigation | Common error source | Small | **P1** |
| 4 | Expose constant node creation tool | Reduces 3 calls to 1 | Small | **P1** |
| 5 | Validation error hints | Better DX | Tiny | **P2** |
| 6 | Dedicated `set_library_metadata` tool | Clean API | Small | **P2** |
| 7 | Metadata persistence in save | Possible bug | Investigate | **P2** |

**With items 1 and 2 fixed**, the library creation workflow becomes:

1. `open_document` (template)
2. `create_function_from_expression`
3. Rewire `main` (create_function_call_node + create_link)
4. `validate_model`
5. `export_to_library(keep_scaffold=true)`

Five steps, no Python workarounds, no metadata patching.

**With items 3 and 4 also fixed**, step 3 simplifies further since
`create_function_call_node` auto-sets resourceid and
`create_constant_nodes_for_missing_parameters` handles wiring constants.
