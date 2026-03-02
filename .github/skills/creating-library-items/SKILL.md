---
name: creating-library-items
description: >-
  Create reusable 3MF library entries for the Gladius implicit modeling
  application using MCP tools. Use when adding new SDF primitives, modifiers,
  or other reusable functions to the Gladius library. Covers the snippet-based
  workflow (preferred), expression-based creation, graph construction, validation,
  rendering, and export with scaffold and metadata.
metadata:
  author: gladius
  version: "2.0"
  requires: gladius-mcp-server
---

# Creating 3MF Library Items with Gladius MCP

A library entry is a `.3mf` file stored in `~/.local/share/gladius/library/<category>/`
containing:

- A **tagged function** — the importable SDF function (e.g. twist, cylinder).
- A **scaffold** — `main` function, mesh, levelset, and build item so the entry
  renders standalone.
- **Metadata** — `gladius:library-functions` (comma-separated resource IDs) and
  `gladius:library-description`.

Reference entries live in the library under categories like `primitives`,
`modifiers`, `csg`, `blending`, `lattices`, etc. Inspect them with
`get_library_entry_info` to see the pattern.

## Workflow Overview

1. Create a new document (loads template with stock functions)
2. Define functions using `set_program_snippet` (preferred) or individual tools
3. Validate the model
4. Render for visual verification
5. Export with `export_to_library(keep_scaffold=true)`

## Step 1 — Create Document

```
create_document()
```

This loads `template.3mf` with ~25 stock functions (box, sphere, torus, etc.)
available for use.

Optionally inspect the current state:

```
get_program_snippet()     # see all functions as code
get_3mf_structure()       # see resource IDs, build items, meshes
```

## Step 2 — Define Functions

### Preferred: Snippet-based (`set_program_snippet`)

This is the most powerful approach. Write all functions as GLSL-like code in a
single listing. Each function block needs a header comment with name and ID.

**Header format:**

```glsl
// Function: <display_name> (ID: <resource_id>)
<return_type> <display_name>_<resource_id>(<args>) {
  <body>
}
```

**Snippet syntax supports:**

- Types: `float`, `vec3`
- Operators: `+`, `-`, `*`, `/`
- Functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`,
  `abs`, `exp`, `log`, `pow`, `mod`, `fmod`, `min`, `max`, `clamp`, `dot`,
  `cross`, `length`, `sign`, `round`, `floor`, `ceil`, `step`, `fract`
- Vector ops: `vec3(x, y, z)`, component access `.x .y .z`
- Variable assignments: `float v = expr;`
- `if (cond) expr1 else expr2` (converted to select nodes)
- Cross-function calls: `functionName_ID(args)` to call other functions
- Multiple outputs: `(float a, vec3 b) funcName_ID(...) { a = expr; b = expr; }`
- `[root]` annotation to mark root functions: `// Function: main (ID: 3) [root]`

**Example — Twist modifier with box demo:**

```glsl
// Function: twist (ID: 1)
vec3 twist_1(vec3 pos, float twist_rate) {
  float angle = pos.z * twist_rate;
  float c = cos(angle);
  float s = sin(angle);
  return vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);
}

// Function: box (ID: 4)
float box_4(vec3 b, vec3 pos) {
  float v0 = abs(pos) - vec3(0.5, 0.5, 0.5) * b;
  return length(max(v0, vec3(0, 0, 0))) + min(0, max(v0.x, max(v0.y, v0.z)));
}

// Function: main (ID: 3) [root]
float main_3(vec3 pos) {
  return box_4(vec3(30, 30, 60), twist_1(pos, 0.1));
}
```

The tool returns the normalized snippet (with CSE-optimized variable names).
Functions not mentioned in the snippet are preserved unchanged.

**Important:** Use IDs from the template. The template has `main` at ID 3 and
`box` at ID 4. Check `get_program_snippet()` output for available IDs. New
functions can reuse IDs of template functions you want to replace.

### Alternative: Single function (`create_function_from_snippet`)

For creating one new function without touching the rest:

```
create_function_from_snippet(
    name="twist",
    snippet="float angle = pos.z * twist_rate;\nfloat c = cos(angle);\nfloat s = sin(angle);\nreturn vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);",
    arguments=[{"name": "pos", "type": "vec3"}, {"name": "twist_rate", "type": "float"}],
    output_type="vec3"
)
```

Then wire `main` to call it using `set_function_snippet` or `set_program_snippet`.

### Simple math: `create_function_from_expression`

For single-expression SDFs (no `length()`, `vec3()`, or multi-line):

```
create_function_from_expression(
    name="cylinder",
    expression="max(sqrt(pos.x*pos.x + pos.y*pos.y) - radius, abs(pos.z) - height)",
    arguments=[
        {"name": "pos", "type": "vec3"},
        {"name": "radius", "type": "float"},
        {"name": "height", "type": "float"}
    ]
)
```

**Expression limitations:** No `length()`, `vec3()`, `^` (use `pow()`), no
comments, no semicolons. Use `create_function_from_snippet` for anything beyond
basic math.

## Step 3 — Validate

```
validate_model()
```

Expected: `graph_ok: true`, `compile_ok: true`.

**Common errors:**

- `use of undeclared identifier` — a function input is not connected, or a
  cross-function call references a non-existent function ID.
- Syntax errors in snippet — check return type matches body (float vs vec3).

## Step 4 — Render

Quick render with auto camera:

```
render_to_file(output_path="/tmp/preview.png", width=512, height=512)
```

Or with explicit camera control:

```
get_optimal_camera_position()
render_with_camera(
    output_path="/tmp/preview.png",
    width=512, height=512,
    eye_position=[0, -100, 50],
    target_position=[0, 0, 0]
)
```

## Step 5 — Export to Library

```
export_to_library(
    function_id=1,
    category="modifiers",
    name="twist",
    description="Twists a shape around the Z axis by rotating XY coordinates based on Z position and twist rate.",
    keep_scaffold=true
)
```

The `function_id` is the resource ID of the tagged function (not `main`).
`keep_scaffold=true` preserves the full document structure so the entry can
render standalone.

This automatically embeds a thumbnail and stamps library metadata.

## Step 6 — Verify

```
get_library_entry_info(category="modifiers", name="twist")
```

Check that the tagged function has correct inputs/outputs and description.

Test importing into a fresh document:

```
create_document()
import_library_entry(category="modifiers", name="twist")
```

## Guidelines

- **Always use `keep_scaffold=true`** when exporting. Without it, the entry
  won't render standalone.
- **Check `get_program_snippet()` first** to see existing function IDs in the
  template before defining your snippet.
- **Use meaningful parameter names** like `pos`, `radius`, `twist_rate` —
  not `a`, `b`, `c`.
- **main should demonstrate the function** with reasonable default parameter
  values that produce a visible shape.
- **Position modifiers return `vec3`**, SDF shapes return `float`. Match the
  return type to the function's role.
- **Cross-function calls use `name_ID` syntax** — e.g., `box_4(...)` calls the
  function named "box" at resource ID 4.
- **Template function IDs you can reuse:** ID 1 (honeycomb by default), and
  others listed in `get_program_snippet()` output.
- **After export, the description cannot be changed** with `set_library_metadata`
  if it was already set. Delete and re-export to change it.

## Library Categories

| Category | Contents |
|----------|----------|
| `primitives` | Basic shapes: box, sphere, torus, capsule, cylinder |
| `modifiers` | Shape transforms: shell, offset, round, twist, extrude |
| `csg` | Boolean operations (union, difference, intersection) |
| `blending` | Smooth boolean operations |
| `lattices` | Infill patterns: gyroid, honeycomb |
| `2d_primitives` | 2D SDF shapes |
| `mechanical` | Gears, threads |
| `noise` | Noise-based functions |

## Quick Tool Reference

| Tool | Purpose |
|------|---------|
| `create_document` | New document from template |
| `get_program_snippet` | Read all functions as code |
| `set_program_snippet` | Write multiple functions at once |
| `get_function_snippet` | Read one function's code |
| `set_function_snippet` | Write one function's code |
| `create_function_from_snippet` | Create new function from code |
| `create_function_from_expression` | Create from simple math expression |
| `validate_model` | Check graph + OpenCL compilation |
| `render_to_file` | Quick render to PNG |
| `render_with_camera` | Render with camera control |
| `get_optimal_camera_position` | Auto-frame the model |
| `generate_thumbnail` | Generate 256px preview |
| `export_to_library` | Export to library with metadata |
| `get_library_entry_info` | Inspect a library entry |
| `import_library_entry` | Import library entry into document |
| `list_library` | List all library categories and entries |
| `remove_unused_resources` | Clean up unused resources |
