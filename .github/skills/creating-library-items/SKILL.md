---
name: creating-library-items
description: >-
  Build and refine implicit (SDF) models and reusable 3MF library entries in
  Gladius via the MCP server. Use when authoring or composing primitives,
  modifiers, components, scenes, or whole objects; when adding library entries;
  when a model renders slowly or a scene is compute intensive; when reviewing or
  refactoring a graph made of large monolithic functions; and when choosing
  between existing library parts and hand-written SDF math. Covers model
  structure rules, WebGPU render-cost budgets, graph measurement, anti-patterns,
  and the snippet workflow.
metadata:
  author: gladius
  version: "5.1"
  requires: gladius-mcp-server
---

# Creating 3MF Library Items with Gladius MCP

A library entry is a `.3mf` in the user library
(`sago::getDataHome()/gladius/library/<category>/` — `~/Library/Application
Support/gladius/library/` on macOS, `~/.local/share/gladius/library/` on Linux)
holding a **tagged function** (the importable SDF), a **scaffold** (`main`,
mesh, levelset, build item, so it renders standalone), and metadata
(`gladius:library-functions` resource IDs, `gladius:library-description`,
`gladius:library-tags`). The shipped library is copied into the user library on
startup; shipped entries are read-only.

**Before authoring any non-trivial model, read
[MODELING-GUIDE.md](MODELING-GUIDE.md)** — how the graph is compiled, what
actually costs time on WebGPU, and the anti-pattern catalog.

## Model Structure Rules

Agents consistently produce slow, unmaintainable graphs. Follow these rules on
every model, not just library entries.

1. **Every primitive is its own function.** Never inline a sphere/box/cylinder
   SDF into `main` or a component — import it or write it once and call it.
2. **Every component is its own function.** A leg, a wall, a tree, a gear —
   one function each, shape parameters as arguments.
3. **`main` only places, combines, and colors.** It must read as a parts list:
   transform, call, `min`/`max`. Arithmetic beyond that means it is too deep.
4. **Never duplicate shape math.** Three trees = one `tree(...)` called three
   times with different offsets, not `treeA`, `treeB`, `treeC`.
5. **Parameterize, do not hard-code.** `tree(pos, height, trunkRadius)`, not a
   function with `22.0` and `3.5` baked in.
6. **One function = one shape or one operation.**
7. **Reuse before you write.** `list_library(query="...")` first — the library
   already has box, sphere, cylinder, capsule, cone, torus, hexagon, rounded
   box, tube, twist, symmetry, polar repetition, and smooth unions.

**Why this matters for speed:** Gladius flattens the whole graph into one
function before compiling to WGSL (`GraphFlattener`, `src/Document.cpp:850`) and
clones per *call site*. Splitting a monolith into functions does **not** by
itself reduce render cost — the flat node count does. Modularity buys
maintainability; fewer/simpler shapes buy frame time. Do both, and never report
a refactor as a performance win without measuring the flat node count.

## Render Cost (WebGPU)

On Apple Silicon WebGPU is the only render path. The evaluator runs once per
ray-march step **plus** ~83 more times per hit pixel (4 normal, 8 AO, 2 × ≤32
soft-shadow, 6 refinement, 1 color taps):

```
frame cost ≈ flat node count reachable from main × evaluation count
```

Budget: keep `main`'s flattened node count under ~400; over ~800 the preview
degrades badly. Exact tap counts, how to measure, and the
`tea_light_house_v2` (347 nodes, fast) vs `v3` (1006 nodes, slow) case study are
in [MODELING-GUIDE.md](MODELING-GUIDE.md) §2–§4.

## MCP Server Modes

`--headless` runs with no window (best for automation/CI); no flag opens the GUI
so a human sees live updates. Both expose the same tools except the UI tools
(`ui_click`, `ui_dump_windows`, `ui_dump_items`, `capture_screenshot`), which
need UI mode.

After rebuilding the MCP binary you must force-kill and restart it — the old
binary stays resident as `(deleted)`, and the process ignores `SIGTERM`:

```bash
rtk proxy sh -c "pkill -9 -f 'gladiusmcp'"
```

Use `rtk proxy`, not an output-filtering wrapper: the server speaks MCP over
stdio, so never pipe its stdout through `rtk err` or another filter. Restart it
from your MCP client configuration.

### Build variants: pure WebGPU MCP builds

Some MCP binaries are built with `GLADIUS_ENABLE_OPENCL=OFF` (e.g. the
`macos-webgpu-releaseWithDebug` preset). In that build `list_library`,
`get_library_entry_info`, `import_library_entry`, `create_library_entry`,
`export_to_library`, `set_library_metadata`, the bin tools, `evaluate_function`,
and `validate_model`'s compile phase all return
`"... unavailable in a pure WebGPU MCP build."` Renders
(`render_to_file`, `render_with_camera`, `generate_thumbnail`,
`get_optimal_camera_position`) still work — they fall back to WebGPU.

So on such a build you cannot create or import library entries through MCP. Do
not retry or assume a bad argument list: check `get_status()` and the build's
`GLADIUS_ENABLE_OPENCL` cache entry, then run the OpenCL-enabled binary or read
the inventory from the filesystem. The repo's `gladius/library/` has identical
contents and is the authoritative inventory; inspect any entry with
`open_document` + `get_program_snippet`, or unzip the `.3mf` and read
`3D/3dmodel.model` (see [LIBRARY.md](LIBRARY.md)).

## Workflow Overview

### Quick Path (Recommended): one-step creation

```
create_library_entry(
    category="operations",
    name="twist",
    program_snippet="// Function: twist (ID: 1)\nvec3 twist_1(vec3 pos, float twist_rate) {\n  float angle = pos.z * twist_rate;\n  float c = cos(angle);\n  float s = sin(angle);\n  return vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);\n}\n\n// Function: main (ID: 3) [root]\n(float shape) main_3(vec3 pos) {\n  shape = box_4(vec3(30, 30, 60), twist_1(pos, 0.1));\n}",
    function_id=1,
    description="Twists a shape around the Z axis",
    tags=["modifier", "transform", "twist"]
)
```

Required: `name`, `category`, `program_snippet`, `function_id`, `description`.
Optional: `tags`, `overwrite`. The tool creates a document from the template,
applies the snippet, adds a default color output to `main` if missing, validates
compilation and bounding box, renders a thumbnail, exports with scaffold and
metadata, then re-checks the written file. On failure it returns `usage_example`
showing the correct call format.

The snippet must contain **both** the library function and a `main` using
named-output syntax: `(float shape) main_3(vec3 pos)`, not `float main_3(...)`.

### Full-control Path

Create a document → define functions → validate → render → export. Use it when
composing multiple interacting functions, customizing the scaffold, or iterating
before export.

### Step 1 — Create Document

`create_document()` loads `template.3mf` with ~25 stock functions (box, sphere,
torus, …). Inspect with `get_program_snippet()` (all functions as code) and
`get_3mf_structure()` (resource IDs, build items, arguments, output types).

### Step 2 — Define Functions

#### Preferred: `set_program_snippet`

Write all functions as GLSL-like code in one listing. Each block needs a header
comment with name and ID:

```glsl
// Function: <display_name> (ID: <resource_id>)
<return_type> <display_name>_<resource_id>(<args>) { <body> }
```

**Syntax:** types `float`, `vec3`; operators `+ - * /`; functions `sin cos tan
asin acos atan atan2 sqrt abs exp log pow mod fmod min max clamp dot cross
length sign round floor ceil step fract`; `vec3(x, y, z)` and `.x .y .z`;
`float v = expr;` assignments; `if (cond) expr1 else expr2` (→ select nodes);
cross-function calls `name_ID(args)`; multiple outputs
`(float a, vec3 b) func_ID(...)`; `[root]` to mark root functions.

```glsl
// Function: twist (ID: 1)
vec3 twist_1(vec3 pos, float twist_rate) {
  float angle = pos.z * twist_rate;
  float c = cos(angle);
  float s = sin(angle);
  return vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);
}

// Function: main (ID: 3) [root]
(float shape) main_3(vec3 pos) {
  shape = box_4(vec3(30, 30, 60), twist_1(pos, 0.1));
}
```

`box_4` is the template's box — reused, not redefined. Redefining a primitive you
already have is exactly what rule 1 forbids.

The tool returns the normalized snippet (CSE-optimized variable names);
functions not mentioned are preserved unchanged. Use IDs from the template
(`main` = 3, `box` = 4) — check `get_program_snippet()`; new functions may reuse
the ID of a function you are replacing. Wrong `main` syntax fails on export with
"function has no output named 'shape'".

#### Alternative: `create_function_from_snippet`

For one new function without touching the rest:

```
create_function_from_snippet(
    name="twist",
    snippet="float angle = pos.z * twist_rate;\nfloat c = cos(angle);\nfloat s = sin(angle);\nreturn vec3(c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z);",
    arguments=[{"name": "pos", "type": "vec3"}, {"name": "twist_rate", "type": "float"}],
    output_type="vec3"
)
```

Then wire `main` to call it with `set_function_snippet` / `set_program_snippet`.
For a single-expression SDF use `create_function_from_expression` (no `length()`,
`vec3()`, or `^` — use `pow()`; no comments or semicolons).

#### Output naming and verification

Name the output by meaning: `vec3 result` for position/domain transforms
(twist, bend, rotation, repetition), `float shape` for SDFs and CSG, `vec3 color`
for color. `output_type="vec3"` sets the type but does **not** guarantee the
identifier — the single-function tool can still default to `shape`. Check
`get_function_snippet().outputs` before declaring the item done; a transformation
must expose `result`, not `shape`.

Renaming an existing output means updating its identifier and display name, the
matching outputs on every calling FunctionCall node, and their consumer
references. Leave scalar SDF outputs (`box.shape`, `main.shape`) alone — the
levelset references main's `shape` channel. Then reopen, validate, render, and
test a fresh import; for native package edits write the 3MF with lib3mf, since
ZIP integrity alone does not prove Gladius can load it.

### Step 3 — Evaluate (optional)

`evaluate_function(function_id=1, samples=[{"pos": [0, 0, 0], "radius": 5.0}])`
runs the function on the GPU (OpenCL) at sample points — `[x,y,z]` arrays for
`vec3`, numbers for `float`. Use it to confirm the SDF is ~0 on the surface. Not
available in pure WebGPU builds; verify with a small render instead.

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

```
render_to_file(output_path="/tmp/preview.png", width=512, height=512)
```

For an explicit camera, call `get_optimal_camera_position()` first and pass
`eye_position` / `target_position` to `render_with_camera`. Renders fall back to
WebGPU, so they work even when the library tools do not.

### Step 6 — Export to Library

```
export_to_library(
    function_id=1,
    category="operations",
    name="twist",
    description="Twists a shape around the Z axis",
    keep_scaffold=true,
    overwrite=false
)
```

`function_id` is the tagged function's resource ID (not `main`). Always
`keep_scaffold=true` or the entry won't render standalone. The export embeds a
thumbnail, stamps metadata, and aborts if the bounding box is degenerate or the
thumbnail render fails.

### Step 7 — Set Tags and Verify

```
set_library_metadata(function_ids=[1], description="Twists a shape around the Z axis",
                     tags=["modifier", "transform", "twist"])
```

Pass `category` and `name` to update tags on an **existing** entry without
opening it. Then `get_library_entry_info(...)` to check the tagged function's
inputs, outputs, description, and tags, and test a fresh import with
`create_document()` + `import_library_entry(...)`.

## Change Tracking

`get_status()` gives `server_time`; `get_changes_since(since="<that ISO time>")`
returns the functions created or modified since — use it to detect external (UI)
edits, confirm what your own calls changed, and coordinate between agents.

## Searching the Library

```
list_library()                        # all categories and entries
list_library(category="operations")   # one category
list_library(query="twist")           # case-insensitive match on name, description, tags
```

## Composing Library Entries

Build complex objects from existing entries rather than writing everything from
scratch. A metric nut = `primitives/hexagon` + `mechanical/iso_external_thread` —
but `mechanical/metric_nut` already exists, so check before rebuilding it.

**1. Discover.** Search with several queries — names, descriptions and tags are
all matched:

```
list_library(query="thread")       # → mechanical/iso_external_thread
list_library(query="hexagon")      # → primitives/hexagon
list_library(query="nut")          # → mechanical/metric_nut
list_library(query="smooth union") # → csg/smooth_union, csg/smooth_union_quadratic
```

**2. Inspect signatures.** `get_library_entry_info(category="primitives",
name="hexagon")` returns `functions` (`name`, `resource_id`, `inputs`,
`outputs`, `is_tagged`) and `description`. The tagged function is what gets
imported. Note parameter names, types **and order** — order is not consistent
across entries, never guess it:

| Entry | Tagged signature |
|-------|------------------|
| `primitives/hexagon` | `hexagon_17(vec3 pos, float size)` |
| `primitives/box` | `box_4(vec3 b, vec3 pos)` |
| `mechanical/metric_nut` | `metric_nut_1(vec3 pos, float m_size)` |
| `mechanical/iso_external_thread` | `isoExternalThread_21(float diameter, float pitch, vec3 pos, float thread_length)` |

**3. Import.**

```
create_document()
import_library_entry(category="mechanical", name="iso_external_thread")  # → ID 21
import_library_entry(category="primitives", name="hexagon")              # → ID 17
```

Dependencies are imported automatically. Resource IDs are assigned dynamically —
use the IDs from the import response, not from `get_library_entry_info`.
`get_program_snippet()` then shows the full assembly.

**4. Compose in `main`** — placement and boolean combination only, no primitive
math:

```
set_program_snippet(snippet="""
// Function: main (ID: 3) [root]
(float shape, vec3 color) main_3(vec3 pos) {
  float hex = max(hexagon_17(pos, 5.5), abs(pos.z) - 2.4);
  float thread = isoExternalThread_21(6.0, 1.0, pos, 4.8);
  shape = max(hex, thread);
  color = vec3(0.6, 0.6, 0.65);
}
""")
```

Then `validate_model()`, `render_to_file(...)`, `export_to_library(...)`.

### SDF Composition Patterns

| Pattern | Code | Use case |
|---------|------|----------|
| Union | `min(a, b)` | Join two shapes |
| Intersection | `max(a, b)` | Keep only the overlap |
| Subtraction | `max(a, -b)` | Cut B from A |
| 2D → 3D extrusion | `max(sdf2d, abs(pos.z) - h)` | Extrude a 2D SDF along Z |
| Shell/hollow | `abs(sdf) - thickness` | Hollow a solid |
| Offset | `sdf - radius` | Grow a shape |
| Bounded placement | `max(part, region)` | Restrict a detail to a region |

For smooth/decorative booleans import `csg/smooth_union` or
`csg/smooth_union_quadratic` instead of writing a polynomial blend inline.

### Tips for Composition

- **2D primitives need extrusion.** Entries like `primitives/hexagon` return a
  2D SDF; extrude with `max(sdf2d(pos), abs(pos.z) - halfHeight)`.
- **Template functions are available too** — `box_4`, `sphere_5`, `capsule_7`,
  `smoothUnion_48`, `repetitionPolar_40`, `symmetryX_31` and more (full list in
  [LIBRARY.md](LIBRARY.md)).
- **New helper functions can live in the snippet** alongside imported ones; give
  them a fresh ID higher than any existing.
- **Keep `main` the only `[root]`** and let it orchestrate the components.
- **Transform by argument, not by rewriting the shape** — pass an offset/scale,
  or import `operations/rotationX|Y|Z`, `operations/symmetryX|Y|Z`,
  `operations/twist`, `operations/polar_repetition`.

### Worked example: a scene, done right

```glsl
// Function: tree (ID: 50) — one parameterized component, reused for every tree
float tree_50(vec3 p, float height, float trunkRadius, float spread) {
  float trunk = box_4(vec3(trunkRadius, trunkRadius, height), p);
  float canopy = sphere_5(p - vec3(0, 0, height), spread);
  return min(trunk, canopy);
}

// Function: main (ID: 3) [root] — a parts list: place, call, combine
(float shape, vec3 color) main_3(vec3 pos) {
  float ground = box_4(vec3(120, 120, 4), pos - vec3(0, 0, -2));
  float house  = houseWalls_30(pos);
  float treeA  = tree_50(pos - vec3(-30, 20, 0), 22.0, 1.2, 6.0);
  float treeB  = tree_50(pos - vec3( 28, 15, 0), 18.0, 1.0, 5.0);
  float treeC  = tree_50(pos - vec3( 34,-18, 0), 26.0, 1.4, 7.0);
  shape = min(min(ground, house), min(treeA, min(treeB, treeC)));
  color = vec3(0.75, 0.75, 0.78);
}
```

The anti-pattern this replaces: `tea_light_house_v3.3mf` has `treeA`, `treeB`
and `treeC` as three separate functions, 22 direct calls in `main`, and 1006
flattened nodes — 2.9× the cost of the 347-node `v2`. Full diff and anti-pattern
catalog in [MODELING-GUIDE.md](MODELING-GUIDE.md) §4–§5.

## Guidelines

- **Prefer `create_library_entry`** for single-function items; use the
  full-control path when composing or iterating.
- **Always `keep_scaffold=true`** when exporting, and **add tags** so the entry
  is discoverable via `list_library`.
- **Check `get_program_snippet()` first** for the IDs already in the assembly.
- **Name things properly:** parameters `pos`, `radius`, `twist_rate` (not `a`,
  `b`, `c`); functions after the thing they build (`tree`, `houseWalls`), not
  `step1` or `tmp_shape`. `main` must demonstrate the function with defaults that
  produce a visible shape.
- **Outputs:** modifiers expose `vec3 result`, SDFs `float shape`, color `vec3
  color`. Cross-function calls use `name_ID` — `box_4(...)` is "box" at ID 4.
- **Never submit a monolithic function.** If a body contains more than one shape
  or more than ~30 lines of math, split it. Review `get_program_snippet()` before
  exporting and confirm `main` is a parts list, not a scene description.
- **Error responses include `usage_example`** with the expected parameter format,
  so you can self-correct on failure.

### Snippet Authoring Tips

- `get_program_snippet` prints arguments with an `in_` prefix (`in_pos`); the
  parser accepts both `in_pos` and `pos`.
- **Only functions that exist in the assembly are callable.** There are no
  built-in `fBox`/`fSphere` helpers. Call a template function or imported entry;
  write raw SDF math only when none exists — and then as its own function, never
  inline in `main`.
- Property access on calls works: `repetitionPolar_40(20, in_pos, 10).x`.
- Multi-output functions use tuple syntax `(float a, vec3 b)` and assign each
  output in the body.
- The parser normalizes your code — intermediate variables may be inlined. Re-read
  with `get_program_snippet()`.

## Library Categories

Five categories ship: `primitives`, `csg`, `operations`, `lattices`,
`mechanical`. There is **no** `modifiers`, `blending`, `2d_primitives`, or
`noise` category — modifiers live in `operations`, smooth booleans in `csg`.
Never invent a category name when exporting.

The full entry list, tagged signatures, template function IDs, and how to read an
entry's signature when `list_library` is unavailable are in
[LIBRARY.md](LIBRARY.md) — load it before composing from parts.

## Slow Model? Check This First

Do not start by tweaking camera or step settings.

1. `get_program_snippet()` and count the nodes reachable from `main` after
   inlining. That number, not the function count, is the cost (<400 good,
   >800 bad). [MODELING-GUIDE.md](MODELING-GUIDE.md) §3 has a ready-to-run
   analyzer for a saved `.3mf`.
2. Look for the structural anti-patterns: flat union of every object in `main`,
   copy-paste component variants, hand-placed duplicates instead of
   `operations/*` repetition, `sin`/`cos` surface displacement, `mod`/`floor`
   repetition. Fixes are catalogued in §5.
3. A domain mesh with `meshbboxonly="true"` is **not** a cost factor — it is
   never loaded as geometry and contributes one `BoxMinMax` node. Never shrink
   one for performance (§6).

## Tool Reference

The complete per-tool listing is in [TOOLS.md](TOOLS.md). The tools you need most
are: `get_status`, `create_document`, `get_program_snippet`,
`set_program_snippet`, `create_function_from_snippet`, `set_function_snippet`,
`validate_model`, `render_to_file`, `export_to_library`, `create_library_entry`,
`list_library`, `import_library_entry`, `get_library_entry_info`,
`set_library_metadata`, `get_changes_since`, `remove_unused_resources`.
