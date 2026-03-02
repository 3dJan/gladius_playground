# Implicit CAD Library Function Catalog

A catalog of functions for a comprehensive implicit (SDF-based) CAD library,
organized by category. Each entry lists the function name, a brief description,
and its expected parameters.

---

## 1. Basic Primitives

| Function | Description | Parameters |
|----------|-------------|------------|
| `sphere` | Sphere centered at origin | `pos:vec3`, `radius:float` |
| `box` | Axis-aligned box | `pos:vec3`, `sizeX:float`, `sizeY:float`, `sizeZ:float` |
| `rounded_box` | Box with rounded edges | `pos:vec3`, `sizeX:float`, `sizeY:float`, `sizeZ:float`, `radius:float` |
| `cylinder` | Cylinder along Z axis | `pos:vec3`, `radius:float`, `height:float` |
| `capsule` | Cylinder with hemispherical caps | `pos:vec3`, `radius:float`, `height:float` |
| `torus` | Torus in the XY plane | `pos:vec3`, `majorRadius:float`, `minorRadius:float` |
| `cone` | Cone along Z axis | `pos:vec3`, `radius:float`, `height:float` |
| `truncated_cone` | Cone with flat top | `pos:vec3`, `bottomRadius:float`, `topRadius:float`, `height:float` |
| `ellipsoid` | Axis-aligned ellipsoid | `pos:vec3`, `radiusX:float`, `radiusY:float`, `radiusZ:float` |
| `hexagonal_prism` | Regular hexagonal prism | `pos:vec3`, `radius:float`, `height:float` |
| `triangular_prism` | Equilateral triangular prism | `pos:vec3`, `sideLength:float`, `height:float` |
| `half_space` | Infinite half-space (plane) | `pos:vec3`, `normalX:float`, `normalY:float`, `normalZ:float`, `offset:float` |
| `infinite_cylinder` | Infinite cylinder along Z | `pos:vec3`, `radius:float` |

---

## 2. Lattice Structures (TPMS)

Triply Periodic Minimal Surfaces — commonly used for lightweight infill,
heat exchangers, and biomedical scaffolds.

| Function | Description | Parameters |
|----------|-------------|------------|
| `gyroid` | Gyroid surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `schwarz_p` | Schwarz Primitive surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `schwarz_d` | Schwarz Diamond surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `neovius` | Neovius surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `lidinoid` | Lidinoid surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `iwp` | I-WP (Schoen's I-Wrapped Package) | `pos:vec3`, `scale:float`, `thickness:float` |
| `frd` | Fischer-Koch S surface | `pos:vec3`, `scale:float`, `thickness:float` |
| `split_p` | Split P surface | `pos:vec3`, `scale:float`, `thickness:float` |

### Lattice Variants

| Function | Description | Parameters |
|----------|-------------|------------|
| `bcc_lattice` | Body-centered cubic strut lattice | `pos:vec3`, `cellSize:float`, `strutRadius:float` |
| `octet_truss` | Octet truss lattice | `pos:vec3`, `cellSize:float`, `strutRadius:float` |
| `kelvin_cell` | Kelvin cell (truncated octahedron) foam | `pos:vec3`, `cellSize:float`, `thickness:float` |
| `voronoi_lattice` | Voronoi-based stochastic lattice | `pos:vec3`, `cellSize:float`, `thickness:float`, `randomness:float` |

---

## 3. Transformations

Functions that modify the position input before evaluating the inner shape.

| Function | Description | Parameters |
|----------|-------------|------------|
| `translate` | Offset a shape | `pos:vec3`, `shape:float`, `offsetX:float`, `offsetY:float`, `offsetZ:float` |
| `rotate_x` | Rotate around X axis | `pos:vec3`, `shape:float`, `angle:float` |
| `rotate_y` | Rotate around Y axis | `pos:vec3`, `shape:float`, `angle:float` |
| `rotate_z` | Rotate around Z axis | `pos:vec3`, `shape:float`, `angle:float` |
| `scale_uniform` | Uniform scale | `pos:vec3`, `shape:float`, `factor:float` |
| `scale_nonuniform` | Non-uniform scale | `pos:vec3`, `shape:float`, `scaleX:float`, `scaleY:float`, `scaleZ:float` |
| `mirror_x` | Mirror across YZ plane | `pos:vec3`, `shape:float` |
| `mirror_y` | Mirror across XZ plane | `pos:vec3`, `shape:float` |
| `mirror_z` | Mirror across XY plane | `pos:vec3`, `shape:float` |
| `twist_z` | Twist around Z axis | `pos:vec3`, `shape:float`, `rate:float` |
| `bend_z` | Bend along Z axis | `pos:vec3`, `shape:float`, `radius:float` |
| `taper_z` | Linear taper along Z | `pos:vec3`, `shape:float`, `topScale:float`, `bottomScale:float` |
| `elongate` | Elongate a shape along axes | `pos:vec3`, `shape:float`, `extendX:float`, `extendY:float`, `extendZ:float` |

---

## 4. Repetition / Pattern

| Function | Description | Parameters |
|----------|-------------|------------|
| `repeat_infinite` | Infinite repetition along all axes | `pos:vec3`, `shape:float`, `cellX:float`, `cellY:float`, `cellZ:float` |
| `repeat_finite` | Finite repetition (clamped) | `pos:vec3`, `shape:float`, `cellX:float`, `cellY:float`, `cellZ:float`, `countX:float`, `countY:float`, `countZ:float` |
| `linear_array` | Linear array along one axis | `pos:vec3`, `shape:float`, `spacing:float`, `count:float`, `axisX:float`, `axisY:float`, `axisZ:float` |
| `circular_array` | Circular array around Z | `pos:vec3`, `shape:float`, `count:float`, `radius:float` |
| `grid_array` | 2D grid in XY plane | `pos:vec3`, `shape:float`, `spacingX:float`, `spacingY:float`, `countX:float`, `countY:float` |

---

## 5. CSG Operators

Boolean operations for constructive solid geometry.

### Sharp Booleans

| Function | Description | Parameters |
|----------|-------------|------------|
| `union` | Union of two shapes | `pos:vec3`, `shapeA:float`, `shapeB:float` |
| `intersection` | Intersection of two shapes | `pos:vec3`, `shapeA:float`, `shapeB:float` |
| `subtraction` | Subtract B from A | `pos:vec3`, `shapeA:float`, `shapeB:float` |

### Smooth Booleans

| Function | Description | Parameters |
|----------|-------------|------------|
| `smooth_union` | Smooth (filleted) union | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float` |
| `smooth_intersection` | Smooth intersection | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float` |
| `smooth_subtraction` | Smooth subtraction | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float` |

### Chamfer Booleans

| Function | Description | Parameters |
|----------|-------------|------------|
| `chamfer_union` | Chamfered union | `pos:vec3`, `shapeA:float`, `shapeB:float`, `size:float` |
| `chamfer_intersection` | Chamfered intersection | `pos:vec3`, `shapeA:float`, `shapeB:float`, `size:float` |
| `chamfer_subtraction` | Chamfered subtraction | `pos:vec3`, `shapeA:float`, `shapeB:float`, `size:float` |

### Stair-Step Booleans

| Function | Description | Parameters |
|----------|-------------|------------|
| `stairs_union` | Stair-step union | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float`, `steps:float` |
| `stairs_intersection` | Stair-step intersection | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float`, `steps:float` |
| `stairs_subtraction` | Stair-step subtraction | `pos:vec3`, `shapeA:float`, `shapeB:float`, `radius:float`, `steps:float` |

---

## 6. Shape Modifiers

Operations that modify an existing SDF distance field.

| Function | Description | Parameters |
|----------|-------------|------------|
| `shell` | Hollow out a shape | `pos:vec3`, `shape:float`, `thickness:float` |
| `offset` | Grow/shrink by constant distance | `pos:vec3`, `shape:float`, `distance:float` |
| `round` | Round all edges | `pos:vec3`, `shape:float`, `radius:float` |
| `onion` | Concentric shells | `pos:vec3`, `shape:float`, `thickness:float` |
| `extrude_z` | Extrude a 2D SDF along Z | `pos:vec3`, `shape2D:float`, `height:float` |
| `revolve_z` | Revolve a 2D profile around Z | `pos:vec3`, `shape2D:float`, `offset:float` |
| `displacement` | Displace surface with noise/function | `pos:vec3`, `shape:float`, `displacementField:float` |
| `clamp_distance` | Clamp SDF range | `pos:vec3`, `shape:float`, `minDist:float`, `maxDist:float` |

---

## 7. Blending & Interpolation

| Function | Description | Parameters |
|----------|-------------|------------|
| `linear_blend` | Linear interpolation between two shapes | `pos:vec3`, `shapeA:float`, `shapeB:float`, `t:float` |
| `gradient_blend` | Blend driven by a spatial gradient | `pos:vec3`, `shapeA:float`, `shapeB:float`, `gradientAxis:float`, `low:float`, `high:float` |
| `radial_blend` | Blend driven by distance from Z axis | `pos:vec3`, `shapeA:float`, `shapeB:float`, `innerRadius:float`, `outerRadius:float` |

---

## 8. Noise & Textures

Scalar fields useful for displacement, density modulation, or surface texture.

| Function | Description | Parameters |
|----------|-------------|------------|
| `perlin_noise` | 3D Perlin noise | `pos:vec3`, `scale:float`, `amplitude:float` |
| `simplex_noise` | 3D Simplex noise | `pos:vec3`, `scale:float`, `amplitude:float` |
| `fbm_noise` | Fractal Brownian Motion (layered noise) | `pos:vec3`, `scale:float`, `amplitude:float`, `octaves:float`, `lacunarity:float`, `gain:float` |
| `sine_wave` | Sine wave along an axis | `pos:vec3`, `frequency:float`, `amplitude:float`, `axisX:float`, `axisY:float`, `axisZ:float` |
| `checkerboard` | 3D checkerboard pattern | `pos:vec3`, `cellSize:float` |
| `radial_wave` | Concentric ripple pattern | `pos:vec3`, `frequency:float`, `amplitude:float` |

---

## 9. Standard Mechanical Components

Commonly used engineering shapes defined as implicit functions.

### Fasteners

| Function | Description | Parameters |
|----------|-------------|------------|
| `thread_external` | External (bolt) thread | `pos:vec3`, `majorDiameter:float`, `pitch:float`, `height:float` |
| `thread_internal` | Internal (nut) thread | `pos:vec3`, `majorDiameter:float`, `pitch:float`, `height:float` |
| `hex_nut` | Hexagonal nut body | `pos:vec3`, `wrenchSize:float`, `height:float`, `boreDiameter:float` |
| `hex_bolt_head` | Hexagonal bolt head | `pos:vec3`, `wrenchSize:float`, `height:float` |
| `countersink` | Countersink recess | `pos:vec3`, `topDiameter:float`, `boreDiameter:float`, `depth:float` |
| `counterbore` | Counterbore recess | `pos:vec3`, `boreDiameter:float`, `counterboreDiameter:float`, `boreDepth:float`, `counterboreDepth:float` |

### Structural

| Function | Description | Parameters |
|----------|-------------|------------|
| `tube` | Hollow cylinder (pipe) | `pos:vec3`, `outerRadius:float`, `innerRadius:float`, `height:float` |
| `i_beam` | I-beam cross-section, extruded | `pos:vec3`, `width:float`, `height:float`, `flangeThickness:float`, `webThickness:float`, `length:float` |
| `c_channel` | C-channel cross-section | `pos:vec3`, `width:float`, `height:float`, `flangeThickness:float`, `webThickness:float`, `length:float` |
| `bracket_l` | L-bracket | `pos:vec3`, `width:float`, `height:float`, `thickness:float`, `depth:float` |

### Gears & Motion

| Function | Description | Parameters |
|----------|-------------|------------|
| `spur_gear` | Involute spur gear profile | `pos:vec3`, `module:float`, `teeth:float`, `height:float`, `pressureAngle:float` |
| `helix` | Helical coil (spring) | `pos:vec3`, `coilRadius:float`, `wireRadius:float`, `pitch:float`, `turns:float` |
| `cam_disc` | Cam profile disc | `pos:vec3`, `baseRadius:float`, `liftHeight:float`, `thickness:float` |

### Joints & Connections

| Function | Description | Parameters |
|----------|-------------|------------|
| `hinge_knuckle` | Hinge knuckle (half) | `pos:vec3`, `pinRadius:float`, `outerRadius:float`, `width:float` |
| `dovetail_slot` | Dovetail slot cross-section | `pos:vec3`, `topWidth:float`, `bottomWidth:float`, `depth:float`, `length:float` |
| `snap_hook` | Cantilever snap-fit hook | `pos:vec3`, `width:float`, `height:float`, `thickness:float`, `hookDepth:float` |

### Fluidics & Channels

| Function | Description | Parameters |
|----------|-------------|------------|
| `round_channel` | Internal round channel | `pos:vec3`, `radius:float`, `length:float` |
| `elbow_channel` | 90° round elbow | `pos:vec3`, `channelRadius:float`, `bendRadius:float` |
| `manifold_y` | Y-shaped channel junction | `pos:vec3`, `channelRadius:float`, `trunkLength:float`, `branchLength:float`, `branchAngle:float` |

---

## 10. 2D Primitives (for Extrude / Revolve)

Base profiles intended for use with `extrude_z` or `revolve_z`.

| Function | Description | Parameters |
|----------|-------------|------------|
| `circle_2d` | 2D circle | `pos:vec3`, `radius:float` |
| `rect_2d` | 2D rectangle | `pos:vec3`, `width:float`, `height:float` |
| `rounded_rect_2d` | 2D rounded rectangle | `pos:vec3`, `width:float`, `height:float`, `radius:float` |
| `polygon_2d` | Regular polygon | `pos:vec3`, `radius:float`, `sides:float` |
| `star_2d` | Star shape | `pos:vec3`, `outerRadius:float`, `innerRadius:float`, `points:float` |
| `arc_2d` | Circular arc segment | `pos:vec3`, `radius:float`, `thickness:float`, `angle:float` |
| `slot_2d` | Stadium/slot shape | `pos:vec3`, `length:float`, `radius:float` |

---

## Priority Recommendation

For initial library population, start with the following order:

1. **Core primitives** — sphere, box, rounded_box, cylinder, capsule, torus, cone
2. **CSG operators** — union, intersection, subtraction, smooth variants
3. **Shape modifiers** — shell, offset, round
4. **Key TPMS** — gyroid, schwarz_p, schwarz_d
5. **Transformations** — translate, rotate, mirror, twist
6. **Repetition** — repeat_finite, linear_array, circular_array
7. **Mechanical** — tube, thread_external, hex_nut, helix
8. **Blending** — linear_blend, gradient_blend
9. **Remaining categories** as needed
