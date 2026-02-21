# Creating 3MF Library Items with Gladius MCP

This guide describes how to create reusable implicit-function library entries for the
Gladius application using MCP tools. A library entry is a `.3mf` file containing:

- A **tagged function** — the importable SDF function (e.g. cylinder, sphere).
- A **`main` function** — a demo that calls the tagged function so the entry
  renders when opened standalone.
- A **mesh** (bounding-box domain), **levelset**, and **build item** that form
  the rendering scaffold.
- **Metadata** — `gladius:library-functions` (comma-separated resource IDs of
  tagged functions) and `gladius:library-description`.

Reference files live in `gladius/library/primitives/` (box, capsule, hexagon,
sphere, torus). Examine them with `get_library_entry_info` or `get_3mf_structure`
to see the pattern.

---

## High-Level Workflow

1. Open an existing library entry as a template (preserves scaffold).
2. Create the new function — either from an expression or by building a graph.
3. Rewire `main` to call the new function.
4. Remove unused resources.
5. Validate (`compile: true`).
6. Render to PNG for visual verification.
7. Export with `export_to_library(keep_scaffold=true)`.

---

## Step 1 — Open a Template

Open an existing primitive to reuse its scaffold:

```
open_document(path="/absolute/path/to/gladius/library/primitives/sphere.3mf")
```

Then inspect the structure:

```
get_3mf_structure()          # see all resource IDs
get_function_graph(function_id=3)   # inspect main
```

Identify:
- `main` function ID (typically 3)
- The mesh ID (typically 100)
- The levelset ID (typically 101)
- Existing tagged function IDs to replace/extend

---

## Step 2 — Create the Function

### Option A: Expression-based (simple SDFs)

Use `create_function_from_expression` for straightforward math expressions.

**Expression syntax rules:**
- Supported operators: `+`, `-`, `*`, `/`
- Supported functions: `sqrt`, `abs`, `max`, `min`, `sin`, `cos`, `tan`,
  `pow`, `exp`, `log`
- Supported constants: `pi`, `e`
- **Not supported**: `length()`, `vec2()`, `^` (use `pow`), comments, semicolons

**Coordinate access depends on the arguments list:**

| Arguments | Coordinate access | Example |
|-----------|------------------|---------|
| None (empty list) | Implicit `x`, `y`, `z` | `sqrt(x*x + y*y) - 10` |
| Custom (e.g. `pos:vec3`) | Component access | `sqrt(pos.x*pos.x + pos.y*pos.y) - radius` |

When using custom arguments, position **must** be `vec3` type and accessed
via `.x`, `.y`, `.z`.

**Example — cylinder with radius and height parameters:**

```
create_function_from_expression(
    name="cylinder",
    expression="max(sqrt(pos.x*pos.x + pos.y*pos.y) - radius, abs(pos.z) - height)",
    arguments=[
        {"name": "pos",    "type": "vec3"},
        {"name": "radius", "type": "float"},
        {"name": "height", "type": "float"}
    ]
)
```

The response contains the new `resource_id`. Note this ID — you need it later.

### Option B: Graph construction (complex functions)

For functions that compose other library functions (e.g. intersection of a Box
and a Gyroid), build a node graph manually:

1. **Create the function call nodes:**

   ```
   create_function_call_node(
       function_id=<target_function>,
       target_function_id=<target_function>,
       referenced_function_id=<library_function_to_call>
   )
   ```

   This creates **two nodes**: a FunctionCall node and a Resource node.
   The response includes both node IDs.

2. **Set the `resourceid` on each Resource node** (critical!):

   ```
   set_parameter_value(
       function_id=<target_function>,
       node_id=<resource_node_id>,
       parameter_name="resourceid",
       value=<library_function_resource_id>
   )
   ```

   Without this, validation fails with "undeclared identifier" errors.

3. **Create constant nodes** for parameters — either manually or use the
   convenience tool:

   ```
   create_constant_nodes_for_missing_parameters(
       function_id=<target_function>,
       node_id=<function_call_node_id>
   )
   ```

   This analyses the node's unconnected inputs and creates constant nodes
   with default values, linking them automatically. To do this manually
   instead:

   ```
   create_node(function_id=<target_function>, node_type="ConstantScalar")
   create_node(function_id=<target_function>, node_type="ConstantVector")
   ```

   Then set their values:

   ```
   set_parameter_value(function_id=..., node_id=..., parameter_name="value", value=0.2)
   ```

4. **Link nodes** — connect inputs to outputs:

   ```
   create_link(
       function_id=<target_function>,
       source_node_id=<from_node>,
       source_port_name="pos",
       target_node_id=<to_node>,
       target_parameter_name="pos"
   )
   ```

   Typical wiring pattern:
   - `Input:pos` → each function call's `pos` parameter
   - Constants → function call parameters (e.g. `wallthickness`, `radius`)
   - Function call outputs (e.g. `shape`, `result`) → combiner inputs → `Output:shape`

---

## Step 3 — Rewire `main`

The `main` function must call the new tagged function so the library entry
renders correctly when opened.

1. **Inspect the current main graph:**

   ```
   get_function_graph(function_id=3)
   ```

2. **Delete existing internal nodes** (keep Input node 1 and Output node 2):

   ```
   delete_node(function_id=3, node_id=<old_node>)
   ```

3. **Create a function call to the new function:**

   ```
   create_function_call_node(
       function_id=3,
       target_function_id=3,
       referenced_function_id=<new_function_resource_id>
   )
   ```

4. **Set the `resourceid`** on the new Resource node.

5. **Create constants** for the function's parameters and set their default
   values (e.g. `radius=10`, `height=20`). Or use
   `create_constant_nodes_for_missing_parameters` to auto-create them.

6. **Link everything:**
   - `Input:pos` → FunctionCall `pos`
   - Constants → FunctionCall parameters
   - FunctionCall `shape` → `Output:shape`

7. **Clean up:**

   ```
   remove_unused_resources()
   ```

---

## Step 4 — Validate

```
validate_model(compile=true)
```

Expected result: `compile_ok: true`, `graph_ok: true`.

**Common errors:**
- `use of undeclared identifier` — a Resource node is missing its `resourceid`,
  or a function input is not connected. The validator will suggest using
  `create_constant_nodes_for_missing_parameters` to fix unconnected inputs.
- Graph cycle or missing links — check that Output receives a value.

---

## Step 5 — Render for Visual Verification

Generate a thumbnail or render to check the shape:

```
render_with_camera(
    output_path="/tmp/library_preview.png",
    width=512,
    height=512,
    camera_position={"x": 0, "y": -100, "z": 50},
    look_at={"x": 0, "y": 0, "z": 0}
)
```

Or use `get_optimal_camera_position()` first to auto-frame the object, then
pass the returned position to `render_with_camera`.

You can also use `generate_thumbnail()` for a quick check.

---

## Step 6 — Export to Library

Use `export_to_library` with `keep_scaffold=true` to save the complete
library entry including the demo scaffold (build item, levelset, mesh, main):

```
export_to_library(
    function_id=<tagged_function_resource_id>,
    category="primitives",
    name="cylinder",
    description="A cylinder with configurable radius and height",
    keep_scaffold=true
)
```

This automatically:
- Stamps library metadata (`gladius:library-functions` and `gladius:library-description`)
- Generates and embeds a thumbnail
- Saves to `~/.local/share/gladius/library/<category>/<name>.3mf`

### Alternative: Metadata-first workflow

If you need more control, you can stamp metadata first and then save manually:

```
set_library_metadata(
    function_ids=[<tagged_function_resource_id>],
    description="A cylinder with configurable radius and height"
)
save_document_as(path="~/.local/share/gladius/library/primitives/cylinder.3mf")
```

---

## Step 7 — Verify

Confirm the entry is recognized:

```
get_library_entry_info(category="primitives", name="cylinder")
```

Expected: `is_tagged: true` for the tagged function, correct description,
correct inputs/outputs.

Test importing into a fresh document:

```
create_document()
import_library_entry(category="primitives", name="cylinder")
```

The imported function should appear with a new resource ID and be ready to
use in graphs.

---

## Quick Reference — MCP Tool Parameters

| Tool | Key parameters |
|------|---------------|
| `create_function_from_expression` | `name`, `expression`, `arguments` (list of `{name, type}`) |
| `create_function_call_node` | `function_id`, `target_function_id`, `referenced_function_id` |
| `create_node` | `function_id`, `node_type` (e.g. `ConstantScalar`, `ConstantVector`) |
| `create_constant_nodes_for_missing_parameters` | `function_id`, `node_id`, `auto_connect` (default true) |
| `set_parameter_value` | `function_id`, `node_id`, `parameter_name`, `value` |
| `set_library_metadata` | `function_ids` (array), `description` |
| `create_link` | `function_id`, `source_node_id`, `source_port_name`, `target_node_id`, `target_parameter_name` |
| `delete_node` | `function_id`, `node_id` |
| `delete_link` | `function_id`, `source_node_id`, `source_port_name`, `target_node_id`, `target_parameter_name` |
| `validate_model` | `compile` (boolean) |
| `export_to_library` | `function_id`, `category`, `name`, `description`, `overwrite`, `keep_scaffold` |
| `save_document_as` | `path` |
| `set_parameter` | `model_id`, `namespace`, `node_name`, `parameter_name`, `type`, `value` |

---

## Common Pitfalls

1. **Not using `keep_scaffold=true`** — without this flag, `export_to_library`
   strips the scaffold (build item, levelset, mesh, main), producing a minimal
   file. Always pass `keep_scaffold=true` for full library entries.

2. **Forgetting `resourceid` on Resource nodes** — `create_function_call_node`
   does not auto-set this. Always call `set_parameter_value` immediately after.

3. **Expression syntax with custom arguments** — when passing `arguments`,
   you cannot use bare `x`, `y`, `z`. Use `pos.x`, `pos.y`, `pos.z` with a
   `vec3` argument named `pos`.

4. **Not cleaning up** — after deleting nodes, call `remove_unused_resources()`
   to avoid orphaned resources in the file.
