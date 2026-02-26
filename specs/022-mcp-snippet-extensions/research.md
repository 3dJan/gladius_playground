# Research: MCP Snippet Tool Extensions

**Feature**: 022-mcp-snippet-extensions  
**Date**: 2026-02-26

## Research Findings

### R1: Current State of `get_function_snippet` — Arguments & Output Type

**Question**: Does `get_function_snippet` already return `arguments` and `output_type`?

**Finding**: **Yes, already implemented in feature 021.** The `getFunctionSnippet` method in `FunctionOperationsTool.cpp` (lines 2278–2306) already:
- Iterates Begin node outputs to build an `arguments` JSON array with `{name, type}` pairs
- Inspects End node parameters to determine `output_type` (float or vec3)
- Returns both in the response JSON

**Decision**: FR-001 from spec is already satisfied. Tests should be added to verify this behavior explicitly (no tests currently assert on `arguments`/`output_type` in the response).

**Alternatives considered**: None — the implementation exists, just needs test coverage.

---

### R2: Current State of `set_function_snippet` — Argument Handling

**Question**: Can `set_function_snippet` already accept and process function arguments?

**Finding**: **Yes, already implemented.** The `setFunctionSnippet` method accepts `std::vector<FunctionArgument> const & arguments` and passes them to `convertSnippetToGraph`. The MCPServer.cpp registration (line ~1108) defines the `arguments` schema as an optional array of `{name: string, type: string}` objects.

**Decision**: FR-002 from spec is already satisfied for the set path. The `create_function_from_snippet` registration (line ~959) also accepts arguments. Test coverage needed to verify argument creation on Begin node.

---

### R3: Program Snippet Format — Function Signatures

**Question**: Does `get_program_snippet` include full function signatures (argument names/types) in the output?

**Finding**: **Yes, already implemented.** The `convertProgramToSnippet` method (lines 3405–3467) builds a full function signature from Begin node outputs:
```
// Function: displayName (ID: resourceId)
float uniqueName(vec3 pos, float radius) {
  body...
}
```
Arguments are emitted with type and name. Output type is derived from End node parameters. Multi-output functions use tuple-like syntax: `(float shape, vec3 color) func(...)`.

**Decision**: FR-003 and FR-008 from spec are already satisfied. The format already includes signatures in dependency-sorted order.

---

### R4: Program Snippet Format — Build-Item Metadata

**Question**: Does the program snippet include metadata about which functions are build-item roots?

**Finding**: **Not implemented.** The current `convertProgramToSnippet` method iterates functions in topological order but does not reference build items. Build items are managed separately via `Document::getBuildItems()` which returns `nodes::BuildItems` — a vector of `BuildItem` objects each holding a `ResourceId`. The connection is: a build item's `m_id` references an Object's ResourceId, which in turn references a function through the levelset/mesh chain.

However, the `getProgramSnippet` MCP handler operates at the `Assembly` level, not `Document` level. Assembly doesn't have direct access to build items.

**Decision**: FR-004 and FR-012 require new implementation. Two approaches:
1. **Pass build-item info to `convertProgramToSnippet`**: Add a parameter for root function IDs. Assembly-level code annotates function headers with `// [root]` or similar.
2. **Add metadata at MCP tool level**: The `getProgramSnippet` handler in `FunctionOperationsTool` has access to `m_application` which can retrieve the document and build items. Add a separate `root_functions` array to the JSON response.

Selected: **Option 2** — Keep the converter pure (just code generation) and add metadata at the MCP response level. This follows separation of concerns and avoids coupling the converter to build-item knowledge.

---

### R5: `set_program_snippet` — Argument Parsing

**Question**: Can `set_program_snippet` parse function signatures with arguments?

**Finding**: **Yes, already implemented.** The `setProgramSnippet` parser (lines 3490–3600) uses regex to extract arguments from function signatures: `std::regex sigArgRegex(R"((vec3|float)\s+(\w+))")`. It builds a `std::vector<FunctionArgument>` for each block and passes to `convertSnippetToGraph`. Default is `pos: vec3` if no arguments found.

**Decision**: FR-005 is partially satisfied — bodies and signatures can be parsed. Adding a new function definition works (first pass creates the model, second parses). Removing a function would leave it in the assembly; this may need explicit handling.

---

### R6: Argument Validation

**Question**: Are variables validated against declared arguments?

**Finding**: **Partial.** The snippet parser (`ExpressionToGraphConverter::convertSnippetToGraph`) creates Begin node outputs from the arguments list. If a variable is used in the snippet body but not declared as an argument or assigned locally, the parser will fail to resolve it and `convertSnippetToGraph` returns 0. However, the error message is generic ("Failed to parse snippet") — it doesn't specifically name the undefined variable.

**Decision**: FR-007 and FR-009 need improvement:
- Better error messages naming undefined variables (requires parser enhancement)
- Reserved keyword validation (new check needed)

---

### R7: Graph Tool Deprecation

**Question**: What graph-based tools exist and how should they be deprecated?

**Finding**: The following tools in MCPServer.cpp should be deprecated:
- `get_function_graph` — replaced by `get_function_snippet`
- `set_function_graph` — replaced by `set_function_snippet`
- `create_node` — replaced by snippet editing
- `delete_node` — replaced by snippet editing
- `create_link` — replaced by snippet editing
- `delete_link` — replaced by snippet editing
- `set_parameter_value` — replaced by snippet editing
- `create_function_call_node` — replaced by snippet editing (function calls in code)
- `create_constant_nodes_for_missing_parameters` — replaced by snippet arguments

**Decision**: FR-010 and FR-011 — annotate tool descriptions with "[DEPRECATED]" prefix and add deprecation notice to responses. Tools remain functional. This is a text-only change in MCPServer.cpp tool registrations.

---

### R8: set_program_snippet Function Removal Handling

**Question**: What happens when a function is removed from the program snippet?

**Finding**: Currently `setProgramSnippet` only processes function blocks found in the input. Functions already in the assembly that aren't in the new snippet are **not removed** — they persist. This is arguably correct (non-destructive), but means agents can't delete functions through the program snippet.

**Decision**: Keep current behavior (non-destructive) for now. Document that functions not present in the snippet are preserved. Future enhancement could add explicit delete support.

## Summary of Implementation Gaps

| Spec Requirement | Status | Action Needed |
|-----------------|--------|---------------|
| FR-001: get_function_snippet returns args + output_type | Already implemented | Add test coverage |
| FR-002: set/create accept arguments | Already implemented | Add test coverage |
| FR-003: Program snippet includes signatures | Already implemented | Add test coverage |
| FR-004: Build-item root annotations | **Not implemented** | Add root function metadata to getProgramSnippet response |
| FR-005: set_program_snippet supports signature changes | Mostly implemented | Test coverage; document function-add behavior |
| FR-006: Argument change handling | Already implemented (graph rebuild) | Add explicit test for type-change scenario |
| FR-007: Variable validation | Partial (generic error) | Improve error messages (stretch goal) |
| FR-008: Topological ordering | Already implemented | Covered by existing tests |
| FR-009: Reserved keyword validation | **Not implemented** | Add keyword check in argument processing |
| FR-010: Deprecate graph tools | **Not implemented** | Annotate tool descriptions and responses |
| FR-011: Backward compat for deprecated tools | Already satisfied (tools remain) | No change needed |
| FR-012: Orphan function annotation | **Not implemented** | Include in root-function metadata |
