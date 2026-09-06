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
  version: "4.1"
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
rtk proxy sh -c "pkill -9 -f 'gladiusmcp'"
```

Use `rtk proxy` rather than an output-filtering wrapper because the MCP server
uses stdio protocol traffic. Never pipe its stdout through `rtk err` or another
filter.

Regular `pkill` without `-9` does not work because the process ignores
`SIGTERM`. After killing, trigger a restart from your MCP client configuration.

## Workflow Overview

### Quick Path (Recommended): One-step creation

```
create_library_entry(
    category="modifiers",
    name="twist",
    program_snippet="// Function: twist (ID: 1)\nvec3 twist_1(vec3 pos, float twist_rate) {\n  float angle = pos.z * twist_rate;\n  float c = cos(angle);\n  float s = sin(angle);\n  return vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);\n}\n\n// Function: main (ID: 3) [root]\n(float shape) main_3(vec3 pos) {\n  shape = length(twist_1(pos, 0.1)) - 15.0;\n}",
    function_id=1,
    description="Twists a shape around the Z axis",
    tags=["modifier", "transform", "twist"]
)
```

This single call creates the document, applies the full program snippet
(including a `main` function that demonstrates the library function),
validates compilation and bounding box, renders a thumbnail, and exports
to the library. It fails with a helpful error if compilation, thumbnail,
or bounding box validation fails.

**Important:** The `program_snippet` must include both the library function
AND a `main` function. The main function must use named-output syntax:
`(float shape) main_3(vec3 pos)` — not `float main_3(vec3 pos)`.

### Full-control Path

1. Create a new document → define functions → validate → render → export

Use the full-control path when you need to:
- Compose multiple interacting functions
- Customize the scaffold (main function, camera position)
- Iteratively refine with `evaluate_function` before exporting

## Quick Path Details

`create_library_entry` accepts a full program snippet (same format as
`set_program_snippet`) that must include both the library function and a
`main` function demonstrating it. The tool:

1. Creates a fresh document from the template
2. Applies the program snippet via `set_program_snippet`
3. Adds a default color output to `main` if missing (so the 3MF template's
   volumetric color reference stays valid)
4. Validates compilation (graph + OpenCL) and bounding box
5. Renders a thumbnail
6. Exports with scaffold, metadata, and thumbnail
7. Validates the written file's 3MF references

Required parameters: `name`, `category`, `program_snippet`, `function_id`,
`description`. Optional: `tags` (array of strings), `overwrite` (boolean).

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
(float shape) main_3(vec3 pos) {
  shape = box_4(vec3(30, 30, 60), twist_1(pos, 0.1));
}
```

The tool returns the normalized snippet (with CSE-optimized variable names).
Functions not mentioned in the snippet are preserved unchanged.

**Important:** Use IDs from the template. The template has `main` at ID 3 and
`box` at ID 4. Check `get_program_snippet()` output for available IDs. New
functions can reuse IDs of template functions you want to replace.

**Important:** The `main` function must use named-output syntax
`(float shape) main_3(vec3 pos)` — not `float main_3(vec3 pos)`. The 3MF
template references the output channel "shape" by name. Using the wrong
syntax causes a "function has no output named 'shape'" error on export.

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
    samples=[
        {"pos": [0, 0, 0]},
        {"pos": [10, 0, 0]},
        {"pos": [0, 0, 20]}
    ]
)
```

Each sample is a JSON object mapping argument names to values. Use `[x,y,z]`
arrays for `vec3` arguments and plain numbers for `float` arguments. For
functions with multiple arguments:

```
evaluate_function(
    function_id=1,
    samples=[
        {"pos": [0, 0, 0], "radius": 5.0},
        {"pos": [10, 0, 0], "radius": 5.0}
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
    {"inputs": {"pos": [0, 0, 0]}, "outputs": {"Vector": [0, 0, 0]}},
    {"inputs": {"pos": [10, 0, 0]}, "outputs": {"Vector": [10, 0, 0]}},
    {"inputs": {"pos": [0, 0, 20]}, "outputs": {"Vector": [0, 0, 20]}}
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
    keep_scaffold=true,
    overwrite=false
)
```

The `function_id` is the resource ID of the tagged function (not `main`).
`keep_scaffold=true` preserves the full document structure so the entry can
render standalone. `overwrite=true` replaces an existing entry with the same
name (default: false).

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

You can also update tags on an **existing** library entry without opening it, by
passing `category` and `name`:

```
set_library_metadata(
    function_ids=[1],
    description="hexagon (2d)",
    tags=["primitive", "hexagon", "2d", "polygon", "nut", "bolt", "honeycomb"],
    category="primitives",
    name="hexagon"
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

## Composing Library Entries

Complex objects are best built by combining existing library entries rather
than writing everything from scratch. For example, a metric nut = hexagonal
prism (from `primitives/hexagon`) + ISO internal thread (from
`mechanical/iso-metric-internal-thread`).

### Composition Workflow

#### Step 1 — Discover Components

Search the library for relevant building blocks. Use multiple queries to cast
a wide net — search by function type, domain terms, and related keywords:

```
list_library(query="thread")       # → mechanical/iso-metric-internal-thread
list_library(query="hexagon")      # → primitives/hexagon
list_library(query="union smooth") # → csg/smooth_union, csg/soft_union
```

**Tip:** Descriptions and tags are searched too, so queries like "lattice",
"2d", "boolean", or "decorative" work even if the name doesn't match.

#### Step 2 — Inspect Signatures

For each candidate, inspect its function signature to understand inputs and
outputs:

```
get_library_entry_info(category="primitives", name="hexagon")
```

This returns:
- **`functions`** — all functions in the entry, each with `name`, `resource_id`,
  `inputs` (name + type), `outputs` (name + type), and `is_tagged` (whether
  it's the importable function).
- **`description`** — what the entry does and what parameters mean.

Look at the **tagged function** (`is_tagged: true`) — that's what gets imported.
Note its parameter names and types; you'll call it by `name_ID(args)` after
import.

#### Step 3 — Import Components

Create a fresh document and import each component:

```
create_document()
import_library_entry(category="mechanical", name="iso-metric-internal-thread")
# → imported as isoInternalThread at resource ID 21
import_library_entry(category="primitives", name="hexagon")
# → imported as hexagon at resource ID 17
```

Each import returns the function name and assigned resource ID. Dependencies
(sub-functions like `isoThreadProfile`) are imported automatically.

**Important:** Resource IDs are assigned dynamically — always use the IDs
from the import response, not from `get_library_entry_info`.

#### Step 4 — Check Available Functions

After all imports, inspect the full assembly:

```
get_program_snippet()
```

This shows every function with its ID and signature. Find the imported
functions and note their `name_ID` call syntax (e.g. `hexagon_17(pos, size)`,
`isoInternalThread_21(pos, diameter, pitch, thread_length, wall_thickness)`).

#### Step 5 — Compose in Main

Write a `main` function that calls the imported components. Use standard SDF
composition patterns:

```
set_program_snippet(snippet="""
// Function: main (ID: 3) [root]
(float shape, vec3 color) main_3(vec3 pos) {
  float hex = max(hexagon_17(pos, 5.5), abs(pos.z) - 2.4);
  float thread = isoInternalThread_21(pos, 3.0, 0.5, 4.8, 1.0);
  shape = max(hex, thread);
  color = vec3(0.6, 0.6, 0.65);
}
""")
```

#### Step 6 — Validate, Render, Export

```
validate_model()
render_to_file(output_path="/tmp/preview.png", width=512, height=512)
export_to_library(function_id=..., category="mechanical", name="metric_nut",
    description="...", keep_scaffold=true)
```

### SDF Composition Patterns

When composing shapes in `main`, use these standard patterns:

| Pattern | Code | Use case |
|---------|------|----------|
| Union (join) | `min(a, b)` | Combine two shapes into one |
| Intersection (trim) | `max(a, b)` | Keep only where both overlap |
| Subtraction (cut) | `max(a, -b)` | Remove shape B from shape A |
| 2D → 3D extrusion | `max(sdf2d, abs(pos.z) - h)` | Extrude a 2D SDF along Z axis |
| Shell/hollow | `abs(sdf) - thickness` | Hollow out a solid shape |
| Offset | `sdf - radius` | Grow a shape by radius |

For smooth/decorative booleans, import a CSG operator from the library
(e.g. `smooth_union`, `chamfer_subtraction`, `stairs_union`) and call it
instead of using raw `min`/`max`.

### Tips for Composition

- **Search broadly first.** Before writing any SDF math from scratch, check
  if the library already has it: `list_library(query="cylinder")`,
  `list_library(query="prism")`, etc.
- **2D primitives need extrusion.** Library entries in `2d_primitives` and
  some entries like `hexagon` return 2D SDFs. Extrude them with
  `max(sdf2d(pos), abs(pos.z) - halfHeight)` to make a solid.
- **Template functions are available too.** After `create_document()`, the
  template includes `box_4`, `sphere_5`, `torus_6`, `capsule_7`, `circle_8`,
  `difference_9`, `union_10`, `intersection_11`, and more. Use
  `get_program_snippet()` to see them all.
- **Combine imported + inline functions.** You can write new helper functions
  in the snippet alongside imported ones. Use a fresh ID (higher than any
  existing) for new functions.
- **One main, many components.** Keep the `main` function as the only `[root]`
  — it should orchestrate all the imported components.

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

- **`get_program_snippet` emits `in_` prefix for arguments** — the output shows
  `in_pos`, `in_radius` etc. in function bodies. When writing snippets, you can
  use either form: both `pos` and `in_pos` are accepted by the parser. The `in_`
  prefix exists to prevent collisions between argument and output names.
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
| `create_image3d_function` | Create a function from 3D image data (FunctionFromImage3D) |
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
| `set_library_metadata` | Update entry description and tags (pass `category`/`name` to target a library entry directly) |
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
