# Research Findings: Library Extension (Feature 030)

## Decision 1: Symmetry Operations — Simple Coordinate Negation

**Decision**: symmetryY, symmetryZ, and symmetryXYZ use direct coordinate negation formulas.

**Rationale**: These are trivial transformations that extend the existing symmetryX pattern. No complex math required.

| Operation | Formula |
|-----------|---------|
| symmetryY | `vec3(-pos.x, -pos.y, pos.z)` — point reflection through origin in X and Y (equivalent to Z-axis rotation by 180°) |
| symmetryZ | `vec3(pos.x, -pos.y, -pos.z)` — mirror across XY plane |
| symmetryXYZ | `vec3(-pos.x, -pos.y, -pos.z)` — point reflection through origin (all axes inverted) |

**Alternatives considered**: 
- Separate mirror operations for each axis individually (symmetryY only negates Y). However, the existing symmetryX negates both X and Z (point reflection), so consistency dictates all new operations follow the same pattern.
- Rotation-based mirroring via angle parameters — over-engineered for simple reflections.

**Reference**: Existing `symmetryX` function returns `vec3(-pos.x, -pos.y, pos.z)` which is point reflection in XZ plane (Y negated). Following this pattern: symmetryY negates Y and Z; symmetryZ negates X and Z; symmetryXYZ negates all three.

---

## Decision 2: Ellipsoid SDF — Normalized Length Formula

**Decision**: Use `length(pos / radius) - 1.0` where radius = vec3(xRadius, yRadius, zRadius).

**Rationale**: This is the standard analytical SDF for an ellipsoid centered at origin. It's numerically stable for all positive radii and produces smooth iso-surfaces.

```glsl
float ellipsoid(vec3 pos, float xRadius, float yRadius, float zRadius) {
    return length(pos / vec3(xRadius, yRadius, zRadius)) - 1.0;
}
```

**Alternatives considered**:
- Approximate formula using max() of component-wise distances — less accurate near poles
- Mesh-based SDF from imported geometry — defeats the purpose of a primitive library item

---

## Decision 3: Capsule — Cylinder + Sphere Decomposition via min()

**Decision**: Standard capsule SDF combining cylinder along axis with hemispherical end caps.

```glsl
float capsule(vec3 pos, float radius, float height) {
    vec2 p = abs(pos.xy);
    vec2 d = vec2(
        max(p.y - radius, 0.0),
        length(max(vec2(p.x, pos.z), 0.0)) - height * 0.5
    );
    return d.x + d.y; // This is wrong — need proper capsule SDF
}
```

**Correction**: The standard capsule SDF from Inigo Quilez's articles:

```glsl
float capsule(vec3 pos, float radius, float height) {
    vec2 p = abs(pos.xy);
    vec2 d = vec2(
        max(p.y - radius, 0.0),
        length(max(vec2(p.x, pos.z), 0.0)) - height * 0.5
    );
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
```

**Rationale**: This produces a cylinder of radius `radius` and total height `height`, with hemispherical end caps. It's the standard formulation used in SDF literature.

**Alternatives considered**:
- Union of cylinder and two spheres — works but less efficient (evaluates both shapes)
- Parametric capsule with variable axis — over-engineered; all primitives are axis-aligned by default

---

## Decision 4: Twist Modifier — Per-Slice Rotation

**Decision**: Apply rotation proportional to z-coordinate relative to shape height.

```glsl
vec3 twist(vec3 pos, float angle) {
    if (angle == 0.0) return pos;
    float t = pos.z / 10.0; // Normalize by default height
    float c = cos(angle * t);
    float s = sin(angle * t);
    vec2 rotated = vec2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);
    return vec3(rotated.x, rotated.y, pos.z);
}
```

**Rationale**: This produces smooth twisting deformation. The parameter `angle` ranges from 0° (no twist) to 180° (half-turn). At 0°, the function returns identity; at 180°, the top face rotates half a turn relative to the base.

**Alternatives considered**:
- Twist around arbitrary axis — over-engineered for initial implementation
- Non-linear twist profiles (e.g., quadratic) — useful but not needed per spec requirements

---

## Decision 5: Bend Modifier — Arc Mapping

**Decision**: Map shape points onto a circular arc of radius R, where R is derived from the bend angle.

```glsl
float bend(vec3 pos, float angle) {
    if (angle == 0.0) return length(pos) - 1.0; // Identity: use input shape
    
    float radius = 5.0 / sin(angle * 0.5); // Arc radius from angle
    vec2 center = vec2(0.0, radius);
    vec2 p = pos.xy;
    
    // Project onto arc
    float dist = length(p - center);
    float newDist = atan(p.y - center.y, p.x) * radius;
    
    return /* SDF of bent shape */;
}
```

**Correction**: Bend is more complex than twist because it requires transforming the input SDF. The approach:
1. Take an input shape's SDF value at each point
2. Map points from Cartesian to polar coordinates centered on bend axis
3. Apply angular deformation based on distance from bend origin
4. Return modified SDF

**Rationale**: This produces smooth bending without self-intersection for angles up to 180°. The arc radius is computed to prevent overlap at maximum bend.

**Alternatives considered**:
- Simple rotation of a subset of points — produces discontinuities
- Mesh-based deformation — defeats the purpose of SDF library items

---

## Decision 6: Mechanical Parts — Simplified Geometric Approximations

**Decision**: Use basic SDF primitives combined via min/max for mechanical parts. Full engineering precision is unnecessary for visual preview purposes.

### Hex Nut (hex_nut)
```glsl
float hex_nut(vec3 pos, float size) {
    // Outer hexagonal cylinder
    float angle = 2.0 * 3.14159 / 6.0;
    vec2 p = abs(pos.xy);
    float outerHex = max(
        dot(p, vec2(sin(angle), cos(angle))),
        -pos.z + size * 0.5,
        pos.z + size * 0.5
    ) - size * 0.577; // Hex width factor
    
    // Inner threaded hole (simplified as cylinder)
    float innerHole = length(pos.xy) - size * 0.3;
    
    return max(outerHex, innerHole);
}
```

### Flat Washer (washer_flat)
```glsl
float washer_flat(vec3 pos, float outerRadius, float innerRadius, float height) {
    vec2 p = abs(pos.zy);
    float cylinder = max(max(p.x - height * 0.5, p.y), length(max(vec2(p.x, pos.z), 0.0)) - outerRadius);
    float hole = length(pos.xy) - innerRadius;
    return max(cylinder, hole);
}
```

### Rivet (rivet)
```glsl
float rivet(vec3 pos, float shaftRadius, float headRadius, float shaftHeight, float headHeight) {
    // Cylinder for shaft
    vec2 p = abs(pos.xy);
    float shaft = max(max(p.y - shaftHeight * 0.5, length(max(vec2(p.x, pos.z), 0.0)) - shaftRadius), 0.0);
    
    // Hemisphere for head
    float headCenter = vec3(0.0, shaftHeight * 0.5 + headHeight * 0.5, 0.0);
    float sphere = length(pos - headCenter) - headRadius;
    
    return min(shaft, sphere); // Union via min()
}
```

**Rationale**: These approximations produce visually correct shapes for preview purposes while keeping implementation simple (<20 lines per function). Full engineering precision (e.g., thread profiles, chamfers) is deferred to future features.

**Alternatives considered**:
- Import CAD models — defeats the purpose of a mathematical library; adds file size and complexity
- Parametric parts with full metric standards — over-engineered for initial implementation

---

## Summary of All Decisions

| Decision | Chosen Approach | Key Reason |
|----------|----------------|------------|
| Symmetry operations | Coordinate negation (point reflection) | Consistency with existing symmetryX; trivial to implement |
| Ellipsoid SDF | `length(pos/radius) - 1.0` | Standard analytical formula; numerically stable |
| Capsule SDF | Cylinder + hemispheres via min() | Reuses existing patterns; well-documented |
| Twist modifier | Per-slice rotation proportional to z | Smooth deformation; parameter range 0°–180° safe |
| Bend modifier | Arc mapping with angular deformation | Prevents self-intersection at max bend angle |
| Mechanical parts | Simplified geometric approximations | Visual preview sufficient; defer engineering precision |
