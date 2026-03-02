# MCP Tool Contracts: Snippet Extensions

**Feature**: 022-mcp-snippet-extensions  
**Date**: 2026-02-26

These contracts document the expected MCP tool request/response formats after this feature is implemented. Tools marked with "[EXISTING — no change]" are already implemented and need only test coverage. Tools marked with "[EXTENDED]" require code changes.

---

## get_function_snippet [EXISTING — no change, add tests]

**Description**: Get the GLSL-like code representation of a function's node graph. Returns snippet, arguments, output type, and display name.

### Request
```json
{
  "function_id": 42
}
```

### Response (success)
```json
{
  "success": true,
  "function_id": 42,
  "snippet": "float v0 = length(pos) - radius;\nreturn v0;",
  "display_name": "sphere",
  "arguments": [
    {"name": "pos", "type": "vec3"},
    {"name": "radius", "type": "float"}
  ],
  "output_type": "float"
}
```

### Response (error)
```json
{
  "success": false,
  "function_id": 42,
  "error": "Function not found: 42"
}
```

---

## set_function_snippet [EXISTING — add keyword validation]

**Description**: Replace a function's node graph from a GLSL-like code snippet. Validates arguments, parses snippet, and returns normalized code.

### Request
```json
{
  "function_id": 42,
  "snippet": "return length(pos) - radius;",
  "output_type": "float",
  "arguments": [
    {"name": "pos", "type": "vec3"},
    {"name": "radius", "type": "float"}
  ]
}
```

### Response (success)
```json
{
  "success": true,
  "function_id": 42,
  "snippet": "float v0 = length(pos);\nfloat v1 = v0 - radius;\nreturn v1;"
}
```

### Response (reserved keyword error — NEW)
```json
{
  "success": false,
  "function_id": 42,
  "error": "Invalid argument name 'float': reserved keyword"
}
```

---

## create_function_from_snippet [EXISTING — add keyword validation]

**Description**: Create a new function from a snippet with name, arguments, and output type.

### Request
```json
{
  "name": "sphere",
  "snippet": "return length(pos) - radius;",
  "output_type": "float",
  "arguments": [
    {"name": "pos", "type": "vec3"},
    {"name": "radius", "type": "float"}
  ]
}
```

### Response (success)
```json
{
  "success": true,
  "function_id": 99,
  "snippet": "float v0 = length(pos);\nfloat v1 = v0 - radius;\nreturn v1;"
}
```

---

## get_program_snippet [EXTENDED — add root annotations]

**Description**: Get the entire document as a GLSL-like code listing with all functions in dependency order. Includes metadata about root functions (used by build items).

### Request
```json
{}
```

### Response (success) — EXTENDED format
```json
{
  "success": true,
  "snippet": "// Function: sphere (ID: 10)\nfloat sphere_10(vec3 pos) {\n  return length(pos) - 5.0;\n}\n\n// Function: shell (ID: 20) [root]\nfloat shell_20(vec3 pos) {\n  float v0 = sphere_10(pos);\n  return abs(v0) - 1.0;\n}\n",
  "function_count": 2,
  "root_functions": [20]
}
```

**New fields**:
- `root_functions`: Array of resource IDs for functions that are referenced by build items (scene objects). Empty if no build items exist.
- Function comment headers for root functions include `[root]` annotation: `// Function: shell (ID: 20) [root]`

---

## set_program_snippet [EXISTING — no change]

**Description**: Replace all function graphs from a multi-function code listing.

### Request
```json
{
  "snippet": "// Function: sphere (ID: 10)\nfloat sphere_10(vec3 pos) {\n  return length(pos) - 5.0;\n}\n\n// Function: shell (ID: 20)\nfloat shell_20(vec3 pos) {\n  float v0 = sphere_10(pos);\n  return abs(v0) - 1.0;\n}\n"
}
```

### Response (success)
```json
{
  "success": true,
  "snippet": "<normalized program>",
  "function_count": 2
}
```

---

## Deprecated Graph Tools [NEW — description changes only]

The following tools have their descriptions prefixed with `[DEPRECATED]` and responses include a `deprecated` field:

| Tool | Replacement |
|------|-------------|
| `get_function_graph` | `get_function_snippet` |
| `set_function_graph` | `set_function_snippet` |
| `create_node` | `set_function_snippet` / `create_function_from_snippet` |
| `delete_node` | `set_function_snippet` |
| `create_link` | `set_function_snippet` |
| `delete_link` | `set_function_snippet` |
| `set_parameter_value` | `set_function_snippet` |
| `create_function_call_node` | `create_function_from_snippet` (with function calls in code) |
| `create_constant_nodes_for_missing_parameters` | `set_function_snippet` (with arguments) |

### Deprecation Response Extension

Each deprecated tool's response includes:
```json
{
  "deprecated": true,
  "deprecation_notice": "This tool is deprecated. Use 'get_function_snippet' instead for code-based function access."
}
```

Tool descriptions are updated from:
```
"Get the full JSON graph representation of a function"
```
to:
```
"[DEPRECATED: Use get_function_snippet instead] Get the full JSON graph representation of a function"
```
