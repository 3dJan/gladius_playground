# Quick Start Guide: Library Extension (Feature 030)

## Overview

This guide walks through creating new library items for the Gladius 3MF library. Follow these steps to add symmetry operations, basic primitives, deformation modifiers, and mechanical parts.

---

## Prerequisites

1. **Gladius running**: Start the application with headless mode if testing locally:
   ```bash
   ./scripts/gladius --headless
   ```

2. **MCP tools available**: The following tools are used for library management:
   - `list_library` — Browse existing entries
   - `create_library_entry` — Create a new entry with validation
   - `validate_model` — Validate the 3MF model structure
   - `get_library_entry_info` — Inspect an existing entry

---

## Step 1: Creating a Symmetry Operation (P1)

### Example: symmetry_y

**Step 1.1**: Define the GLSL function in set_program_snippet format:

```glsl
// Function: symmetryY (ID: 1)
vec3 symmetryY(vec3 pos) {
    return vec3(-pos.x, -pos.y, pos.z);
}

// Function: main (ID: 2) [root]
(float shape) main(vec3 pos) {
    // Demonstration: apply symmetry to a sphere
    float radius = 10.0;
    vec3 p = symmetryY(pos);
    shape = length(p) - radius;
}
```

**Step 1.2**: Create the library entry via MCP tool call:

```json
{
  "name": "symmetry_y",
  "category": "operations",
  "program_snippet": "// Function: symmetryY (ID: 1)\nvec3 symmetryY(vec3 pos) {\n    return vec3(-pos.x, -pos.y, pos.z);\n}\n\n// Function: main (ID: 2) [root]\n(float shape) main(vec3 pos) {\n    float radius = 10.0;\n    vec3 p = symmetryY(pos);\n    shape = length(p) - radius;\n}",
  "function_id": 1,
  "description": "Point reflection across YZ plane (mirrors X and Z coordinates)",
  "tags": ["symmetry", "mirror", "transformation", "reflection", "operations"],
  "overwrite": false
}
```

**Step 1.3**: Verify the entry:
- Run `get_library_entry_info` with name="symmetry_y" to confirm creation
- Check that tagged_function_id=1 and has_metadata=true
- Render thumbnail in library browser UI

---

## Step 2: Creating a Basic Primitive (P2)

### Example: ellipsoid

**Step 2.1**: Define the GLSL function:

```glsl
// Function: ellipsoid (ID: 1)
float ellipsoid(vec3 pos, float xRadius, float yRadius, float zRadius) {
    return length(pos / vec3(xRadius, yRadius, zRadius)) - 1.0;
}

// Function: main (ID: 2) [root]
(float shape) main(vec3 pos) {
    shape = ellipsoid(pos, 8.0, 5.0, 3.0);
}
```

**Step 2.2**: Create the library entry:

```json
{
  "name": "ellipsoid",
  "category": "primitives",
  "program_snippet": "// Function: ellipsoid (ID: 1)\nfloat ellipsoid(vec3 pos, float xRadius, float yRadius, float zRadius) {\n    return length(pos / vec3(xRadius, yRadius, zRadius)) - 1.0;\n}\n\n// Function: main (ID: 2) [root]\n(float shape) main(vec3 pos) {\n    shape = ellipsoid(pos, 8.0, 5.0, 3.0);\n}",
  "function_id": 1,
  "description": "Ellipsoid primitive with three independent radii along each axis",
  "tags": ["ellipsoid", "primitive", "sphere", "radius", "geometry"],
  "overwrite": false
}
```

---

## Step 3: Creating a Deformation Modifier (P2)

### Example: twist

**Step 3.1**: Define the GLSL function:

```glsl
// Function: twist (ID: 1)
vec3 twist(vec3 pos, float angle) {
    if (angle == 0.0) return pos;
    float t = pos.z / 10.0;
    float c = cos(angle * t);
    float s = sin(angle * t);
    vec2 rotated = vec2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);
    return vec3(rotated.x, rotated.y, pos.z);
}

// Function: main (ID: 2) [root]
(float shape) main(vec3 pos) {
    float radius = 5.0;
    vec3 twisted = twist(pos, 45.0);
    shape = length(twisted.xy) - radius;
}
```

**Step 3.2**: Create the library entry:

```json
{
  "name": "twist",
  "category": "modifiers",
  "program_snippet": "// Function: twist (ID: 1)\nvec3 twist(vec3 pos, float angle) {\n    if (angle == 0.0) return pos;\n    float t = pos.z / 10.0;\n    float c = cos(angle * t);\n    float s = sin(angle * t);\n    vec2 rotated = vec2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);\n    return vec3(rotated.x, rotated.y, pos.z);\n}\n\n// Function: main (ID: 2) [root]\n(float shape) main(vec3 pos) {\n    float radius = 5.0;\n    vec3 twisted = twist(pos, 45.0);\n    shape = length(twisted.xy) - radius;\n}",
  "function_id": 1,
  "description": "Applies rotational twist around Z axis proportional to height",
  "tags": ["twist", "deformation", "modifier", "rotation", "transformation"],
  "overwrite": false
}
```

---

## Step 4: Creating a Mechanical Part (P3)

### Example: hex_nut

**Step 4.1**: Define the GLSL function:

```glsl
// Function: hex_nut (ID: 1)
float hex_nut(vec3 pos, float size) {
    vec2 p = abs(pos.xy);
    float angle = 2.0 * 3.14159 / 6.0;
    float outerHex = max(
        dot(p, vec2(sin(angle), cos(angle))),
        -pos.z + size * 0.5,
        pos.z + size * 0.5
    ) - size * 0.577;
    float innerHole = length(pos.xy) - size * 0.3;
    return max(outerHex, innerHole);
}

// Function: main (ID: 2) [root]
(float shape) main(vec3 pos) {
    shape = hex_nut(pos, 10.0);
}
```

**Step 4.2**: Create the library entry:

```json
{
  "name": "hex_nut",
  "category": "mechanical",
  "program_snippet": "// Function: hex_nut (ID: 1)\nfloat hex_nut(vec3 pos, float size) {\n    vec2 p = abs(pos.xy);\n    float angle = 2.0 * 3.14159 / 6.0;\n    float outerHex = max(\n        dot(p, vec2(sin(angle), cos(angle))),\n        -pos.z + size * 0.5,\n        pos.z + size * 0.5\n    ) - size * 0.577;\n    float innerHole = length(pos.xy) - size * 0.3;\n    return max(outerHex, innerHole);\n}\n\n// Function: main (ID: 2) [root]\n(float shape) main(vec3 pos) {\n    shape = hex_nut(pos, 10.0);\n}",
  "function_id": 1,
  "description": "Hexagonal nut with threaded hole for M6 metric fastener",
  "tags": ["hex_nut", "mechanical", "fastener", "hardware", "metric"],
  "overwrite": false
}
```

---

## Step 5: Validation Checklist

Before finalizing each entry, verify:

| Check | Criteria | Status |
|-------|----------|--------|
| Name uniqueness | No duplicate in same category | ☐ |
| Tag count | ≥5 unique tags | ☐ |
| Description length | 10–100 characters | ☐ |
| Function return type | float for SDF, vec3 for transforms | ☐ |
| Parameter naming | lowerCamelCase, no reserved names | ☐ |
| Has main root function | ID marked as [root] | ☐ |
| Thumbnail renders | Visible in library browser | ☐ |

---

## Step 6: Testing Combined Operations

After creating individual items, test them combined:

1. **symmetryY + ellipsoid**: Apply symmetry to an ellipsoid shape
2. **twist + cylinder**: Twist a cylinder primitive by 45°
3. **hex_nut + socket_cap_screw**: Combine mechanical parts for assembly preview

---

## Troubleshooting

### Issue: "Function already exists" error
**Solution**: Check if the name is taken in that category. Use `list_library` to browse existing entries.

### Issue: Thumbnail not rendering
**Solution**: Ensure the main function returns a valid shape (not NaN or infinity). Test with `validate_model` tool.

### Issue: Shape appears distorted
**Solution**: Verify parameter ranges. For twist, angle should be 0°–180°. For primitives, radii must be positive.

---

## Next Steps

After creating all library items:
1. Run full test suite: `Run Gladius Tests` task
2. Validate all entries with `validate_model` tool
3. Update user documentation if needed
4. Consider adding to shipped library (set `shipped=true`) for new users
