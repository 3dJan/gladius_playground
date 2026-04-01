# Data Model: Fast Mesh Simplification for Export

**Feature**: 026-fast-mesh-simplification  
**Date**: 2026-03-26

## Entities

### SymMat (Symmetric Matrix 4×4)

Internal data structure for the quadric error metric computation.

| Field | Type | Description |
|-------|------|-------------|
| `m[10]` | `double[10]` | Upper triangle of 4×4 symmetric matrix stored flat |

**Operations**:
- Construct from plane equation `(a, b, c, d)`: `m = p * p^T`
- Add: element-wise addition of two SymMats
- Determinant of 3×3 sub-matrix: used to find optimal collapse vertex
- Evaluate error at point: `v^T * Q * v` (homogeneous coordinates)

**Relationships**: Each vertex accumulates a SymMat from all incident triangle planes.

### VertexInfo

Per-vertex metadata for the flat adjacency structure.

| Field | Type | Description |
|-------|------|-------------|
| `q` | `SymMat` | Sum of quadrics from all incident triangles |
| `start` | `uint32_t` | Index into flat EdgeInfo array where this vertex's entries begin |
| `count` | `uint32_t` | Number of incident triangles (entries in EdgeInfo array) |

**State transitions**: 
- `count == 0` → vertex is deleted
- After collapse: surviving vertex absorbs the other vertex's quadric and edge references

### TriangleInfo

Per-triangle metadata stored alongside the index buffer.

| Field | Type | Description |
|-------|------|-------------|
| `normal` | `Vec3f` | Normalized triangle normal (used for flip detection) |
| `minEdgeIndex` | `uint8_t` | Which of the 3 edges (0, 1, 2) has the minimum collapse cost |

**State transitions**:
- `normal.x > 2.0` → triangle is marked as deleted
- After neighbor collapse: normal is recalculated, minEdgeIndex recomputed

### EdgeInfo

Element of the flat adjacency array. Maps a vertex's incident triangle and which edge it contributes to.

| Field | Type | Description |
|-------|------|-------------|
| `triangleIndex` | `uint32_t` | Index of the incident triangle |
| `edge` | `uint8_t` | Which vertex position (0, 1, 2) within the triangle |

**Relationships**: The flat array is partitioned by vertex — `VertexInfo[v].start` to `VertexInfo[v].start + VertexInfo[v].count` gives all EdgeInfo entries for vertex `v`.

### Error (Priority Queue Element)

Represents a triangle's minimum-cost edge collapse in the priority queue.

| Field | Type | Description |
|-------|------|-------------|
| `value` | `float` | The minimum edge collapse error for this triangle |
| `triangleIndex` | `uint32_t` | Which triangle this error belongs to |

**Relationships**: Stored in the mutable priority queue; keyed by `value`. The `TriangleInfo::minEdgeIndex` identifies which edge within the triangle has this error.

### SimplificationTerminationMode (new enum)

User-selected termination criterion.

| Value | Description |
|-------|-------------|
| `TargetTriangleCount` | Stop when triangle count reaches specified value |
| `TargetReductionPercent` | Stop when reduction percentage is reached |
| `ErrorBounded` | Stop when all remaining collapses exceed error threshold |

### SimplificationMethod (extended enum, io namespace)

Algorithm selection in the export options.

| Value | Description |
|-------|-------------|
| `None` | No simplification (existing) |
| `QemFast` | Fast geometric QEM, CPU-only, single-pass with priority queue (new) |
| `QemSdfAware` | QEM with GPU SDF error evaluation (existing) |

### SimplificationMethod (extended enum, compute namespace)

Internal algorithm selection in the compute pipeline.

| Value | Description |
|-------|-------------|
| `None` | No simplification (existing) |
| `QemFast` | Fast geometric QEM (new) |
| `QemSdfAware` | QEM with GPU SDF evaluation (existing) |

## Entity Relationships

```
SurfaceExtractionOptions
  ├── SimplificationMethod (QemFast | QemSdfAware | None)
  ├── SimplificationTerminationMode
  └── error thresholds, target count, etc.

FastQemSimplifier (operates on indexed_triangle_set)
  ├── VertexInfo[] (one per vertex, contains SymMat quadric)
  ├── TriangleInfo[] (one per triangle, contains normal + minEdgeIndex)
  ├── EdgeInfo[] (flat array, 3× triangle count, partitioned by vertex)
  └── MutablePriorityQueue<Error> (one entry per non-deleted triangle)

Exporter pipeline:
  Model → Extraction (LMC|DC|MDC) → indexed_triangle_set → Simplification → Color Resampling → Write (STL|3MF)
```

## Validation Rules

- Triangle count after simplification must be ≤ input triangle count
- All triangles must have 3 distinct vertex indices (no degenerate triangles)
- No edges shared by more than 2 triangles (manifold)
- All triangle normals must have positive dot product with their pre-collapse normals (no flips)
- Boundary edges (shared by exactly 1 triangle) are not collapsed
- The merged quadric error at the optimal vertex must be below the error threshold
