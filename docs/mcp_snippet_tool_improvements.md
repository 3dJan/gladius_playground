# MCP Snippet Tool Improvements

## Context

While creating an involute gear SDF library entry via MCP tools, several friction points were identified. The snippet tools (`create_function_from_snippet`, `get_function_snippet`, `set_function_snippet`) work well for basic use, but complex tasks like creating parameterized library items require unnecessary workarounds.

## Current State

### What Works

- `create_function_from_snippet` accepts `name`, `snippet`, `output_type`, and `arguments` — full function creation from multi-line GLSL-like code.
- `set_function_snippet` accepts `function_id`, `snippet`, `output_type`, and `arguments` — replaces an existing function's graph from code.
- `get_function_snippet` returns the normalized snippet for a function.
- `get_program_snippet` / `set_program_snippet` support whole-program round-trips.

### Pain Points Discovered

#### 1. `get_function_snippet` does not return `arguments` or `output_type`

**Severity**: High (breaks round-trip workflow)

The tool only returns `snippet` and `display_name`. It does not tell the caller what inputs the function expects (e.g., `pos: vec3`, `x: float, y: float`) or what it outputs (`float` vs `vec3`).

This means:
- An AI agent cannot inspect a function's signature before modifying it.
- Round-tripping (get → modify → set) requires guessing the arguments.
- The caller must separately query the 3MF structure to find input/output types.

**Location**: `gladius/src/mcp/tools/FunctionOperationsTool.cpp`, line ~2219 (`getFunctionSnippet`).

**Fix**: Include `arguments` array and `output_type` in the response JSON, matching the format accepted by `set_function_snippet`.

#### 2. `create_library_entry` only accepts single expressions, not snippets

**Severity**: High (forces multi-step workaround)

The `create_library_entry` tool uses `create_function_from_expression` internally, which only supports single-line expressions. Complex SDFs (like the involute gear) require multi-line snippets with intermediate variables.

Current workaround (4 steps instead of 1):
1. `create_function_from_snippet` — create the function
2. `create_levelset` — make it renderable
3. `set_library_metadata` — tag it for the library
4. `save_document_as` — save to library path

**Fix**: Accept an optional `snippet` parameter in `create_library_entry` (falling back to `expression` for simple cases). When `snippet` is provided, use `createFunctionFromSnippet` internally instead of `createFunctionFromExpression`.

#### 3. No automatic bounding box sizing from SDF

**Severity**: Medium (renders appear empty)

When creating a levelset, the bounding box mesh defaults to 200×200×100 units. If the SDF geometry is much smaller (e.g., a gear with radius 11), the object is a tiny speck in the render. The user/agent must manually figure out the transform to scale/center the object.

**Fix**: After creating a levelset, sample the SDF along axes to find approximate zero-crossings and auto-size the bounding box mesh (or at least report the estimated bounds in the response).

#### 4. Snippet constants are not exposed as UI parameters

**Severity**: Medium (limits usability of library items)

When a snippet declares `float module_size = 1.0;`, this becomes a `ConstantScalar` node in the graph. There's no way to mark it as a "user-editable parameter" that appears in the Gladius property panel.

Users who import the gear library entry must open the code view and edit the snippet to change parameters — they can't adjust them from the node editor's property panel.

**Possible approach**: Support a `param` qualifier syntax (e.g., `param float module_size = 1.0;`) that creates an exposed input parameter node instead of a constant node. This would be a snippet parser extension.

#### 5. `create_function_from_snippet` does not auto-infer `pos` argument

**Severity**: Low (minor convenience)

If a snippet uses `pos.x`, `pos.y`, `pos.z`, it's obvious that `pos` is a `vec3` input. Currently the caller must explicitly pass `arguments: [{name: "pos", type: "vec3"}]`. Omitting it causes a parse failure.

**Fix**: When `arguments` is empty or not provided, scan the snippet for `pos.x`/`pos.y`/`pos.z` usage and auto-add `pos: vec3`. Similarly for common patterns like bare `x`, `y`, `z` variables → add them as scalar inputs.

## Prioritized Recommendations

| Priority | Improvement | Effort | Impact |
|----------|-------------|--------|--------|
| P1 | Return `arguments` + `output_type` from `get_function_snippet` | Small | Enables proper round-trip workflows |
| P1 | Accept `snippet` in `create_library_entry` | Medium | One-step library creation for complex shapes |
| P2 | Auto-size bounding box on levelset creation | Medium | Fixes "invisible render" problem |
| P2 | `param` qualifier for exposed parameters | Medium | Makes library items user-friendly |
| P3 | Auto-infer `pos` argument from snippet | Small | Minor convenience |

## Example: Ideal Workflow for Involute Gear

With improvements P1+P1 implemented, the gear creation would be a single tool call:

```json
{
  "tool": "create_library_entry",
  "params": {
    "name": "involute-gear",
    "category": "mechanical-parts",
    "description": "Involute gear (module=1, z=20, α=20°, b=3mm)",
    "snippet": "float module_size = 1.0;\nfloat num_teeth = 20.0;\n...\nreturn min(gear2d, zInside);",
    "arguments": [{"name": "pos", "type": "vec3"}],
    "output_type": "float"
  }
}
```

Instead of the current 4-step process.
