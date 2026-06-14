# 3MF Library Best Practices

## What Makes a Good Library Item?

### Required Metadata
- **Name**: lower_snake_case, descriptive, no spaces or special chars except hyphens/underscores
- **Description**: One sentence explaining what the item does and key parameters
- **Tags**: Minimum 5 relevant keywords for discoverability (e.g., "primitive", "cone", "taper", "basic")
- **Tagged Function**: Exactly one function marked as tagged. Return type depends on purpose:
  - `float shape` for SDF primitives/modifiers
  - `vec3 result` for position transformations (symmetry, rotation, etc.)
  - `vec3 color` for textures/materials

### Code Quality
- Clean bounding box suitable for thumbnail rendering
- Well-named parameters with meaningful defaults where appropriate
- A `main` function demonstrating typical usage
- No duplicate function definitions (each name should have unique ID)
- Proper dependency ordering in program snippet

### Visual Appeal (for Thumbnails)
- Geometry fills 70-85% of thumbnail frame
- Isometric camera angle showing 3D form clearly
- Consistent lighting: directional light from upper-left, ambient fill
- Neutral background (light gray or white)
- Characteristic features visible at thumbnail size (256x256 minimum)

## Anti-Patterns to Avoid
- Empty entries (no description/tags/tagged functions)
- Missing tags on any entry
- Duplicate function names within same file
- Nearly duplicate entries (e.g., unionWithColor vs union_with_color)

## Naming Conventions
- Primitives: `sphere`, `cylinder`, `cone`, etc.
- Modifiers: `round`, `shell`, `offset` (verb-based)
- Operations: `polar_repetition`, `symmetryX` (noun-based with parameter suffix)
- Mechanical: `helix_spring`, `involute_gear` (compound nouns)

## Category Structure
```
primitives/     - Basic SDF shapes (sphere, cylinder, cone, etc.)
csg/           - Boolean operations (union, intersection, subtraction)
modifiers/     - Shape deformations (round, shell, offset)
operations/    - Domain transformations (symmetryX, polar_repetition)
lattices/      - Periodic structures (gyroid, honeycomb)
mechanical/    - Engineering parts (gear, spring, thread)
textures/      - Procedural textures (WoodTexture)
```
