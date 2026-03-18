---
name: creating-library-items
description: >-
  Create reusable 3MF library entries for the Gladius implicit modeling
  application using MCP tools. Use when adding new SDF primitives, modifiers,
  or other reusable functions to the Gladius library. Covers the one-step
  snippet workflow (preferred), multi-function program editing, function
  evaluation, change tracking, validation, rendering, and export with
  scaffold and metadata.
metadata:
  author: gladius
  version: "3.0"
  requires: gladius-mcp-server
---

# Creating 3MF Library Items with Gladius MCP

A library entry is a `.3mf` file stored in `~/.local/share/gladius/library/<category>/`
containing:

- A **tagged function** — the importable SDF function (e.g. twist, cylinder).
- A **scaffold** — `main` function, mesh, levelset, and build item so the entry
  renders standalone.
- **Metadata** — `gladius:library-functions` (comma-separated resource IDs),
  `gladius:library-description`, and `gladius:library-tags` (comma-separated).

Reference entries live in the library under categories like `primitives`,
`modifiers`, `csg`, `blending`, `lattices`, etc. Inspect them with
`get_library_entry_info` to see the pattern.

## MCP Server Modes

The Gladius MCP server can run in two modes:

- **Headless mode** (`--headless`): No GUI window. Best for automated
  workflows and CI pipelines. Runs fully in the background.
- **UI mode** (no `--headless` flag): Opens the Gladius GUI window. Allows
  human + agent collaboration — the human sees live updates in the viewport
  while the agent makes changes via MCP tools.

Both modes expose the same MCP tool set, except UI interaction tools
(`ui_click`, `ui_dump_windows`, `ui_dump_items`, `capture_screenshot`) are
only available in UI mode.

**Important:** After rebuilding the MCP server binary, you must force-kill and
restart the server for changes to take effect. The old binary stays in memory
as `(deleted)` if not killed. Use:

```bash
pkill -9 -f 'gladiusmcp'
```

Regular `pkill` without `-9` does not work because the process ignores
`SIGTERM`. After killing, trigger a restart from your MCP client configuration.

## Workflow Overview

### Quick Path (Recommended): One-step creation

```
create_library_entry(
    category="modifiers",
    name="twist",
    snippet="vec3 twist_1(vec3 pos, float twist_rate) {\n  float angle = pos.z * twist_rate;\n  float c = cos(angle);\n  float s = sin(angle);\n  return vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);\n}",
    description="Twists a shape around the Z axis",
    tags=["modifier", "transform", "twist"]
)
```

This single call creates the document, compiles the snippet, generates a
scaffold (`main` + mesh + levelset + build item), renders a thumbnail,
validates the bounding box, and exports to the library. It fails with a
helpful error if the thumbnail or bounding box is invalid.

### Full-control Path

1. Create a new document → define functions → validate → render → export

Use the full-control path when you need to:
- Compose multiple interacting functions
- Customize the scaffold (main function, camera position)
- Iteratively refine with `evaluate_function` before exporting

## Quick Path Details

`create_library_entry` accepts a snippet body for a single function. The tool:

1. Creates a fresh document from the template
2. Creates the function via `create_function_from_snippet`
3. Wires `main` to call it with sensible defaults
4. Validates the model (graph + OpenCL compilation)
5. Renders a thumbnail and checks the bounding box
6. Exports with `keep_scaffold=true`

If validation, thumbnail generation, or bounding box checks fail, the tool
returns an error with `usage_example` showing the correct call format.

## Full-control Path

### Step 1 — Create Document

```
create_document()
```

This loads `template.3mf` with ~25 stock functions (box, sphere, torus, etc.)
available for use.

Optionally inspect the current state:

```
get_program_snippet()     # see all functions as code
get_3mf_structure()       # see resource IDs, build items, meshes (includes
                          # arguments, output_type, snippet_preview, constants)
```

### Step 2 — Define Functions

#### Preferred: Snippet-based (`set_program_snippet`)

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

#### Alternative: Single function (`create_function_from_snippet`)

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

#### Simple math: `create_function_from_expression`

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

### Step 3 — Evaluate Functions (Optional)

Use `evaluate_function` to numerically verify a function before exporting:

```
evaluate_function(
    function_id=1,
    sample_points=[
        {"x": 0, "y": 0, "z": 0},
        {"x": 10, "y": 0, "z": 0},
        {"x": 0, "y": 0, "z": 20}
    ]
)
```

**Response:**

```json
{
  "success": true,
  "function_id": 1,
  "function_name": "twist",
  "output_type": "vec3",
  "results": [
    {"point": {"x": 0, "y": 0, "z": 0}, "value": {"x": 0, "y": 0, "z": 0}},
    {"point": {"x": 10, "y": 0, "z": 0}, "value": {"x": 10, "y": 0, "z": 0}},
    {"point": {"x": 0, "y": 0, "z": 20}, "value": {"x": 0, "y": 0, "z": 20}}
  ]
}
```

This runs the function on the GPU via OpenCL — the same pipeline used for
rendering. Use it to:

- Verify SDF values at known points (e.g., surface should be ~0)
- Check that position modifiers transform coordinates correctly
- Debug unexpected rendering results

**Note:** If the OpenCL kernel fails to compile or execute, the tool returns a
diagnostic error. Always `validate_model()` first.

### Step 4 — Validate

```
validate_model()
```

Expected: `graph_ok: true`, `compile_ok: true`.

**Common errors:**

- `use of undeclared identifier` — a function input is not connected, or a
  cross-function call references a non-existent function ID.
- Syntax errors in snippet — check return type matches body (float vs vec3).

### Step 5 — Render

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

### Step 6 — Export to Library

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

This automatically embeds a thumbnail and stamps library metadata. The export
validates the bounding box (rejects degenerate/zero-volume/NaN shapes) and
thumbnail rendering before writing — if either fails, the export aborts with
an error.

### Step 7 — Set Tags and Verify

After export, add tags to help with library search and discovery:

```
set_library_metadata(
    function_ids=[1],
    description="Twists a shape around the Z axis",
    tags=["modifier", "transform", "twist"]
)
```

Verify the entry:

```
get_library_entry_info(category="modifiers", name="twist")
```

Check that the tagged function has correct inputs/outputs, description, and tags.

Test importing into a fresh document:

```
create_document()
import_library_entry(category="modifiers", name="twist")
```

## Change Tracking

Use `get_changes_since` to track what changed during a session:

```
# 1. Record the current timestamp from get_status
get_status()
# → { "server_time": "2024-01-15T10:30:00Z", ... }

# 2. Make changes
set_program_snippet(snippet="...")
create_function_from_snippet(...)

# 3. Query what changed since your timestamp
get_changes_since(since="2024-01-15T10:30:00Z")
```

**Response:**

```json
{
  "success": true,
  "changes": [
    {
      "timestamp": "2024-01-15T10:30:05Z",
      "type": "modified",
      "resource_type": "function",
      "resource_id": "3",
      "display_name": "main"
    },
    {
      "timestamp": "2024-01-15T10:30:10Z",
      "type": "created",
      "resource_type": "function",
      "resource_id": "5",
      "display_name": "twist"
    }
  ]
}
```

This is useful for:
- Checking whether external (UI) edits have modified the document
- Confirming which resources your tool calls actually changed
- Multi-agent collaboration (one agent can see the other's changes)

## Searching the Library

Use `list_library` with an optional query to find existing entries:

```
list_library()                          # all categories and entries
list_library(category="modifiers")      # entries in one category
list_library(query="twist")             # search by name, description, or tags
```

The query performs case-insensitive substring matching across entry names,
descriptions, and tags.

## Guidelines

- **Prefer `create_library_entry`** for single-function items — it handles
  the entire workflow in one call.
- **Use the full-control path** when composing multiple interacting functions
  or iterating on a design.
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
- **Add tags** when exporting to make entries discoverable via `list_library`.
- **Use `evaluate_function`** to verify SDF values before exporting.
- **All error responses include `usage_example`** with the expected parameter
  format, so you can self-correct on failure.

### Snippet Authoring Tips

- **Arguments use `in_` prefix in the body** — if a parameter is named `pos`,
  refer to it as `in_pos` inside the function body.
- **Only use functions that exist in the assembly.** There are no built-in
  SDF primitives like `fBox`, `fSphere`, etc. Write box/sphere SDFs using raw
  math (e.g., `length(pos) - radius` for sphere, `abs(pos) - halfSize` for
  box), or call template functions like `box_4(...)`, `sphere_5(...)`.
- **Property access on cross-function calls works** — e.g.,
  `repetitionPolar_40(20, in_pos, 10).x` accesses the `.x` component of the
  returned `vec3`.
- **Multi-output functions** use tuple syntax: `(float a, vec3 b)` before the
  function name. Assign each output in the body: `a = expr; b = expr;`.
- **`set_program_snippet` preserves unmentioned functions** — you only need to
  include the functions you want to create or modify. Existing functions not
  in the snippet are kept unchanged.
- **The parser normalizes your code** — intermediate variables may be inlined
  into a single expression. Use `get_program_snippet()` to see the normalized
  form after setting.

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

## Complete Tool Reference

### Document Management

| Tool | Purpose |
|------|---------|
| `get_status` | Server status, loaded document info, `server_time` for change tracking |
| `create_document` | New document from template |
| `open_document` | Open an existing `.3mf` file |
| `save_document` | Save current document |
| `save_document_as` | Save current document to a new path |
| `get_3mf_structure` | Full document structure: resources, build items, meshes, function arguments, output types, snippet previews, constants |

### Function Authoring

| Tool | Purpose |
|------|---------|
| `get_program_snippet` | Read all functions as GLSL-like code |
| `set_program_snippet` | Write/update multiple functions at once |
| `get_function_snippet` | Read one function's code by ID |
| `set_function_snippet` | Write one function's code by ID |
| `create_function_from_snippet` | Create new function from multi-line code |
| `create_function_from_expression` | Create from a single math expression |
| `create_levelset` | Create a new levelset resource |
| `modify_levelset` | Modify levelset properties |
| `set_parameter` | Set a function parameter value |
| `create_volumetric_color` | Create a volumetric color function |
| `create_volumetric_property` | Create a volumetric property function |

### Evaluation & Validation

| Tool | Purpose |
|------|---------|
| `evaluate_function` | Run a function on sample points via OpenCL (returns float or vec3 results) |
| `validate_model` | Check graph connectivity + OpenCL compilation |
| `get_model_bounding_box` | Get the model's world-space bounding box |

### Rendering

| Tool | Purpose |
|------|---------|
| `render_to_file` | Render to PNG with auto camera |
| `render_with_camera` | Render with explicit camera position |
| `get_optimal_camera_position` | Compute camera position that frames the model |
| `generate_thumbnail` | Generate a 256px preview thumbnail |
| `capture_screenshot` | Capture the current viewport (UI mode only) |

### Build Items

| Tool | Purpose |
|------|---------|
| `set_build_item_object` | Set which resource a build item references |
| `set_build_item_transform` | Set a build item's transformation matrix |

### Library Management

| Tool | Purpose |
|------|---------|
| `create_library_entry` | One-step: create entry from snippet with scaffold, thumbnail, and metadata |
| `export_to_library` | Export current document's function to library |
| `import_library_entry` | Import a library entry into the current document |
| `list_library` | List categories/entries with optional query filter |
| `get_library_entry_info` | Inspect entry metadata, function info, tags, and snippet |
| `set_library_metadata` | Update entry description and tags |
| `delete_library_entry` | Remove a library entry |

### Change Tracking

| Tool | Purpose |
|------|---------|
| `get_changes_since` | List resources changed since a given ISO-8601 timestamp |

### Utility

| Tool | Purpose |
|------|---------|
| `remove_unused_resources` | Clean up unreferenced resources |

### UI Interaction (UI mode only)

| Tool | Purpose |
|------|---------|
| `ui_click` | Click a UI element by name |
| `ui_dump_windows` | List all open UI windows |
| `ui_dump_items` | List UI items in a window |
