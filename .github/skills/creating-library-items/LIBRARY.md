# Gladius Library Inventory

Verified against `gladius/library/` (the shipped library, identical to the user
library after startup sync). Load this when choosing what to import instead of
writing math. Everything here is importable with `import_library_entry`.

## Categories and entries

| Category | Entries |
|----------|---------|
| `primitives` | `box`, `capped_torus`, `capsule`, `cone`, `cylinder`, `diamond`, `ellipsoid`, `hexagon`, `rounded_box`, `sphere`, `torus`, `triangular_prism`, `truncated_cone` |
| `csg` | `smooth_union`, `smooth_union_quadratic`, `subtraction`, `union`, `union_with_color` |
| `operations` | `polar_repetition`, `rotationX`, `rotationY`, `rotationZ`, `symmetryX`, `symmetryY`, `symmetryZ`, `twist` |
| `lattices` | `bcc_strut_lattice`, `double_walled_gyroid`, `gyroid`, `honeycomb`, `octet_truss_lattice`, `simple_cubic_strut_lattice` |
| `mechanical` | `building_brick_nm`, `brick_8mm_pitch_nm`, `brick_16mm_pitch_nm`, `brick_stud`, `brick_socket_cutter`, `building_brick_fit_coupon`, `plate_8mm_pitch_nm`, `plate_16mm_pitch_nm`, `tile_8mm_pitch_nm`, `tile_16mm_pitch_nm`, `car_coupler_16mm_pitch`, `helix_spring`, `hinge`, `hingemount_X`, `involute_gear`, `iso_external_thread`, `metric_nut`, `socket_cap_screw`, `tube` |

There is **no** `modifiers`, `blending`, `2d_primitives`, or `noise` category —
modifiers live in `operations`, smooth booleans in `csg`. Do not invent category
names when exporting; pick one of the five above or create a deliberate new one.

## Tagged function signatures

Argument order is **not** consistent across entries. Never guess it — read it
from `get_library_entry_info()` or from the import response.

| Entry | Tagged signature |
|-------|------------------|
| `primitives/box` | `box_4(vec3 b, vec3 pos)` |
| `primitives/hexagon` | `hexagon_17(vec3 pos, float size)` — 2D SDF, extrude it |
| `mechanical/metric_nut` | `metric_nut_1(vec3 pos, float m_size)` |
| `mechanical/iso_external_thread` | `isoExternalThread_21(float diameter, float pitch, vec3 pos, float thread_length)` |

The `_N` suffix is the function's resource ID **inside the entry file**. After
import the ID changes, so always re-read it from the import response or
`get_program_snippet()`.

## Reading a signature without MCP

On a pure WebGPU MCP build `list_library` and `get_library_entry_info` are
unavailable. Read the inventory from the filesystem instead:

```bash
ls "$HOME/Library/Application Support/gladius/library"   # macOS
ls ~/.local/share/gladius/library                        # Linux
ls gladius/library                                       # repo copy (authoritative)
```

To get an entry's function names, IDs, and inputs without opening it, unzip the
`.3mf` and read `3D/3dmodel.model`:

```bash
unzip -o -d /tmp/entry gladius/library/primitives/hexagon.3mf
```

Functions are `<i:implicitfunction id="N" displayname="…">` (not `<i:function>`).
Inputs are the `<i:vector>` / `<i:scalar>` children of `<i:in>`, and their
`identifier` attributes are the parameter names in declaration order. Shipped
entries carry no `gladius:` metadata attributes, so description and tags are not
available this way.

Alternatively `open_document(path)` then `get_program_snippet()` shows the whole
program as code.

## Template functions (after `create_document()`)

Fixed IDs, always available, no import needed:

`honeycomb_1`, `box_4`, `sphere_5`, `torus_6`, `capsule_7`, `circle_8`,
`difference_9`, `union_10`, `intersection_11`, `normalize_12`,
`toPolarCoordinates_13`, `gyroid_16`, `hexagon_17`, `finiteRepetition_18`,
`hexagoncolumns_28`, `rotationX_29`, `symmetryX_31`, `symmetryY_32`,
`rotationZ_39`, `repetitionPolar_40`, `toCylindricalCoordinates_46`, `mix_47`,
`smoothUnion_48`, and `main_3`.

Selected signatures:

| Function | Signature |
|----------|-----------|
| `box_4` | `box_4(vec3 b, vec3 pos)` — `b` is the **full size**, not the half-extent |
| `sphere_5` | `sphere_5(vec3 pos, float radius)` |
| `torus_6` | `torus_6(vec3 pos, float radius1, float radius2)` |
| `circle_8` | `circle_8(vec3 pos, float radius)` — 2D disc in XY |

`box_4` computes `abs(pos) - 0.5 * b`, so `box_4(vec3(20, 20, 4), pos)` is a
20 × 20 × 4 mm box centred on `pos`. Passing half-extents silently halves every
dimension — the usual cause of floating tiers and missing cuts. `circle_8` is
flat: extrude it with `max(circle_8(q, r), abs(q.z - h) - h)`.

Some template functions declare their inputs in an unintuitive order (the saved
`<i:in>` list is not the call order). Read the signature from
`get_program_snippet()` before calling anything you have not used before.

Confirm with `get_program_snippet()` before writing a snippet; the template can
be replaced by a previous session's document.
