# MCP Tool Contracts: Graph ↔ Code View

**Phase 1 output for [plan.md](../plan.md)**

These contracts define the MCP tools added by this feature. All tools follow the existing Gladius MCP pattern: JSON-RPC over stdio, `registerTool(name, description, schema, handler)`.

---

## Tool: `get_function_snippet`

Get the GLSL-like code representation of a single function.

### Request Schema

```json
{
  "type": "object",
  "properties": {
    "function_id": {
      "type": "integer",
      "description": "The resource ID of the function to convert to code"
    }
  },
  "required": ["function_id"]
}
```

### Response (success)

```json
{
  "success": true,
  "function_id": 42,
  "function_name": "gyroid_42",
  "display_name": "gyroid",
  "snippet": "float v0 = sin(pos.x) + cos(pos.y);\nreturn v0;",
  "unsupported_nodes": []
}
```

### Response (success with unsupported nodes)

```json
{
  "success": true,
  "function_id": 42,
  "function_name": "gyroid_42",
  "display_name": "gyroid",
  "snippet": "float v0 = sin(pos.x);\n/* unsupported: BoxMinMax */\nreturn v0;",
  "unsupported_nodes": ["BoxMinMax"],
  "warning": "Some nodes could not be converted to code. These appear as comments and cannot be synced back."
}
```

### Response (error)

```json
{
  "success": false,
  "error": "Function with ID 42 not found"
}
```

---

## Tool: `set_function_snippet`

Update an existing function's graph from a GLSL-like code snippet. The existing graph is replaced. On success, the response includes the normalized snippet regenerated from the resulting graph.

### Request Schema

```json
{
  "type": "object",
  "properties": {
    "function_id": {
      "type": "integer",
      "description": "The resource ID of the function to update"
    },
    "snippet": {
      "type": "string",
      "description": "The GLSL-like code to parse and convert to a graph"
    },
    "output_type": {
      "type": "string",
      "enum": ["float", "vec3"],
      "description": "The output type of the function (default: float)",
      "default": "float"
    },
    "arguments": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "name": { "type": "string" },
          "type": { "type": "string", "enum": ["float", "vec3"] }
        },
        "required": ["name", "type"]
      },
      "description": "Function arguments beyond the implicit 'pos' parameter (optional)"
    }
  },
  "required": ["function_id", "snippet"]
}
```

### Response (success)

```json
{
  "success": true,
  "function_id": 42,
  "normalized_snippet": "float v0 = sin(pos.x) + cos(pos.y);\nreturn v0;",
  "node_count": 7,
  "validation_warnings": []
}
```

### Response (parse error)

```json
{
  "success": false,
  "error": "Parse error at line 3: unsupported construct 'for'",
  "line": 3,
  "column": 1
}
```

### Response (unsupported node comment)

```json
{
  "success": false,
  "error": "Cannot sync: snippet contains unsupported node comments: BoxMinMax. Remove these comments or edit the function in the Graph view."
}
```

---

## Tool: `get_program_snippet`

Get the entire document as a single GLSL-like code listing with all functions in dependency order.

### Request Schema

```json
{
  "type": "object",
  "properties": {},
  "required": []
}
```

### Response (success)

```json
{
  "success": true,
  "snippet": "// Function: sphere (ID: 10)\nfloat sphere_10(vec3 pos) {\n  return length(pos) - 1.0;\n}\n\n// Function: gyroid (ID: 42)\nfloat gyroid_42(vec3 pos) {\n  float v0 = sphere_10(pos);\n  return v0 + sin(pos.x);\n}\n",
  "function_count": 2,
  "function_order": [10, 42],
  "unsupported_nodes": {}
}
```

### Response (circular dependency)

```json
{
  "success": false,
  "error": "Circular function call dependency detected: funcA_10 → funcB_20 → funcA_10"
}
```

---

## Internal API Contracts (C++)

These are not MCP-exposed but define the key C++ interfaces.

### `generateUniqueFunctionName`

```cpp
/// Generate a unique GLSL identifier from display name and resource ID.
/// Sanitizes display name: replace non-alnum with '_', collapse consecutive '_',
/// prepend 'f_' if starts with digit.
std::string generateUniqueFunctionName(std::string const& displayName, ResourceId resourceId);
```

### `convertGraphToSnippet` (extended)

Existing signature unchanged. Extended to handle all node types listed in data-model.md.

```cpp
/// Convert a function's node graph to a GLSL-like snippet.
/// Returns the snippet text.
/// Unsupported nodes emit /* unsupported: TypeName */ comments.
static std::string convertGraphToSnippet(
    nodes::Model& model,
    std::vector<FunctionArgument> const& arguments = {},
    FunctionOutput const& output = {});
```

### `convertProgramToSnippet` (new)

```cpp
/// Convert all functions in an assembly to a single code listing.
/// Functions are topologically sorted by call dependencies.
/// Throws if circular dependencies are detected.
static std::string convertProgramToSnippet(nodes::Assembly& assembly);
```

### `CodeView` (new UI class)

```cpp
class CodeView
{
public:
    /// Set the current function to display/edit
    void setFunction(ResourceId functionId, nodes::Model& model, nodes::Assembly& assembly);

    /// Render the code editor and sync button. Returns true if sync was performed.
    bool render();

    /// Check if there are unsaved changes for the current function.
    bool hasUnsavedChanges() const;

    /// Discard unsaved changes (revert to last synced text).
    void discardChanges();

private:
    std::unordered_map<ResourceId, CodeBuffer> m_buffers;
    ResourceId m_currentFunctionId{0};
};
```
