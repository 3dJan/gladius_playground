# Data Model: Library Extension (Feature 030)

## Entities

### LibraryEntry

Represents a single item in the Gladius 3MF library.

| Field | Type | Constraints | Description |
|-------|------|-------------|-------------|
| name | string | `lower_snake_case`, unique within category | Human-readable entry name (e.g., "symmetry_y") |
| category | enum | One of: primitives, csg, modifiers, operations, mechanical, lattices, textures | Library category |
| description | string | Non-empty, ≤100 characters | One-sentence human-readable description |
| tags | string[] | Minimum 5 items, unique entries | Searchable keywords (e.g., "symmetry", "mirror", "transformation") |
| tagged_function_id | integer | Positive, unique within entry | Resource ID of the primary exportable function |
| thumbnail_path | string | Relative path to PNG in library directory | Path to rendered preview image |
| shipped | boolean | Default: false | Whether this item is part of the shipped library |

**Relationships**:
- Each LibraryEntry contains exactly one TaggedFunction (the tagged function)
- A LibraryEntry may contain additional helper functions (referenced by resource_id)

---

### TaggedFunction

The primary exportable function within a library entry. Identified by its resource_id in the 3MF file.

| Field | Type | Constraints | Description |
|-------|------|-------------|------------|
| function_name | string | `lower_snake_case`, unique within entry | Name of the GLSL-like function |
| return_type | enum | "float" or "vec3" | Output type: float for SDF, vec3 for transforms/textures |
| parameters | Parameter[] | Non-empty list | Input parameters with name and type |
| body_snippet | string | ≤20 lines of GLSL-like code | Function body in set_program_snippet format |

**Relationships**:
- Belongs to exactly one LibraryEntry
- May reference other functions within the same entry (helper functions)

---

### Parameter

A single input parameter for a TaggedFunction.

| Field | Type | Constraints | Description |
|-------|------|-------------|------------|
| name | string | `lowerCamelCase` | Parameter name (e.g., "xRadius") |
| type | enum | "float" or "vec3" | GLSL-like type declaration |

---

## New Items Data Model

### Symmetry Operations (operations/)

| Entry Name | Return Type | Parameters | Output |
|------------|-------------|------------|--------|
| symmetry_y | vec3 | `pos: vec3` | Mirrored position across YZ plane |
| symmetry_z | vec3 | `pos: vec3` | Mirrored position across XZ plane |
| symmetry_xyz | vec3 | `pos: vec3` | Point reflection through origin |

### Basic Primitives (primitives/)

| Entry Name | Return Type | Parameters | Output |
|------------|-------------|------------|--------|
| ellipsoid | float | `pos: vec3, xRadius: float, yRadius: float, zRadius: float` | Signed distance to ellipsoid surface |
| capsule | float | `pos: vec3, radius: float, height: float` | Signed distance to capsule (cylinder + hemispheres) |
| diamond | float | `pos: vec3, topRadius: float, bottomRadius: float` | Signed distance to bicone (two cones joined at base) |

### Deformation Modifiers (modifiers/)

| Entry Name | Return Type | Parameters | Output |
|------------|-------------|------------|--------|
| twist | vec3 | `pos: vec3, angle: float` | Transformed position after twisting around Z axis |
| bend | vec3 | `pos: vec3, angle: float` | Transformed position after bending into arc |

### Mechanical Parts (mechanical/)

| Entry Name | Return Type | Parameters | Output |
|------------|-------------|------------|--------|
| hex_nut | float | `pos: vec3, size: float` | SDF of hexagonal nut with threaded hole |
| washer_flat | float | `pos: vec3, outerRadius: float, innerRadius: float, height: float` | SDF of flat annular washer |
| rivet | float | `pos: vec3, shaftRadius: float, headRadius: float, shaftHeight: float, headHeight: float` | SDF of rivet (cylinder + hemisphere head) |

---

## Validation Rules

1. **Name uniqueness**: No two entries in the same category may share a name
2. **Tag minimum**: Each entry must have ≥5 unique tags
3. **Description length**: 10–100 characters per description
4. **Function return type consistency**: 
   - primitives/csg/mechanical/lattices → `float shape` (SDF)
   - modifiers/operations → `vec3 result` (position transform) or `float shape` (if SDF modifier)
   - textures → `vec3 color` (color output)
5. **Parameter naming**: All parameters use `lowerCamelCase`, no reserved GLSL names (pos, normal, etc.) as parameter names without prefix
