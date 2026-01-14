# Data Model: Spatial Tree Mesh SDF

**Feature**: 001-spatial-sdf  
**Date**: 2025-12-29  
**Status**: Complete

## Entities

### 1. MeshBVHNode

GPU-compatible BVH node structure for triangle mesh traversal.

| Field | Type | Description |
|-------|------|-------------|
| bboxMin | float4 | Bounding box minimum (xyz, w unused) |
| bboxMax | float4 | Bounding box maximum (xyz, w unused) |
| leftChild | int | Index of left child (-1 if leaf) |
| rightChild | int | Index of right child (-1 if leaf) |
| primStart | int | First triangle index (leaf nodes only) |
| primCount | int | Number of triangles (leaf nodes only) |

**Size**: 48 bytes (aligned to 16-byte boundary)

**Invariants**:
- If `isLeaf()`: `leftChild == -1 && rightChild == -1 && primCount > 0`
- If internal: `leftChild >= 0 && rightChild >= 0 && primCount == 0`

---

### 2. MeshTriangle

Triangle data with vertex indices for normal lookup.

| Field | Type | Description |
|-------|------|-------------|
| v0 | float3 | First vertex position |
| v1 | float3 | Second vertex position |
| v2 | float3 | Third vertex position |
| vertexIndices | int3 | Indices into vertex normal array |

**Size**: 48 bytes

**Invariants**:
- Winding order is CCW (counter-clockwise) when viewed from outside
- `vertexIndices[i] >= 0` for all i

---

### 3. MeshVertexNormal

Angle-weighted pseudo-normal for sign determination.

| Field | Type | Description |
|-------|------|-------------|
| normal | float3 | Angle-weighted normal (normalized) |
| padding | float | Alignment padding |

**Size**: 16 bytes

**Computation**:
```
For each vertex v:
  normal = vec3(0)
  For each triangle t containing v:
    angle = angle at v in triangle t
    normal += angle * faceNormal(t)
  normal = normalize(normal)
```

---

### 4. ClosestPointResult (query result, not stored)

Result of a closest-point query on the mesh BVH.

| Field | Type | Description |
|-------|------|-------------|
| sqDistance | float | Squared distance to closest point |
| closestPoint | float3 | Position of closest point on mesh |
| featureType | int | 0=face, 1=edge, 2=vertex |
| triangleIndex | int | Index of closest triangle |
| barycentricU | float | Barycentric coordinate u |
| barycentricV | float | Barycentric coordinate v |

**Note**: Not stored in GPU buffer; computed during query.

---

### 5. SpatialMeshData (host-side)

Host-side container for mesh BVH data before serialization.

| Field | Type | Description |
|-------|------|-------------|
| nodes | vector<MeshBVHNode> | BVH node array (root at index 0) |
| triangles | vector<MeshTriangle> | Triangle data in BVH order |
| vertexNormals | vector<MeshVertexNormal> | Angle-weighted vertex normals |
| originalTriangleCount | size_t | Source mesh triangle count |
| boundingBox | BoundingBox | Axis-aligned bounding box |

---

### 6. SpatialMeshResource (host-side)

Resource class that owns `SpatialMeshData` and serializes to `PrimitiveBuffer`.

| Field | Type | Description |
|-------|------|-------------|
| m_key | ResourceKey | Resource identifier |
| m_data | SpatialMeshData | Mesh and BVH data |
| m_payloadData | PrimitiveBuffer | Serialized GPU-ready data |
| m_startIndex | int | Start offset in global buffer |
| m_endIndex | int | End offset in global buffer |

**Inheritance**: `ResourceBase` (same pattern as `VdbResource`)

---

## Primitive Types (additions to types.h)

```cpp
enum PrimitiveType
{
    // ... existing types ...
    
    SDF_SPATIAL_MESH_ROOT = 20,  // Metadata for spatial mesh SDF
    SDF_SPATIAL_MESH_NODES,      // BVH node array
    SDF_SPATIAL_MESH_TRIS,       // Triangle data
    SDF_SPATIAL_MESH_NORMALS,    // Vertex normals for sign
};
```

---

## GPU Buffer Layout

```
Offset  | Content                          | PrimitiveType
--------|----------------------------------|----------------------------
0       | PrimitiveMeta (root descriptor)  | SDF_SPATIAL_MESH_ROOT
+meta   | MeshBVHNode[nodeCount]           | SDF_SPATIAL_MESH_NODES
+nodes  | MeshTriangle[triCount]           | SDF_SPATIAL_MESH_TRIS
+tris   | MeshVertexNormal[vertexCount]    | SDF_SPATIAL_MESH_NORMALS
```

**Root PrimitiveMeta Fields**:
- `primitiveType = SDF_SPATIAL_MESH_ROOT`
- `start = offset to nodes`
- `end = offset past normals`
- `boundingBox = mesh AABB`
- `center = mesh centroid`
- `scaling = 1.0` (no voxel scaling needed)

---

## Relationships

```
┌─────────────────────┐
│ SpatialMeshResource │
│  (ResourceBase)     │
└─────────┬───────────┘
          │ owns
          ▼
┌─────────────────────┐
│   SpatialMeshData   │
└─────────┬───────────┘
          │ contains
          ▼
┌─────────────────────┬─────────────────────┬─────────────────────┐
│ vector<MeshBVHNode> │ vector<MeshTriangle>│vector<MeshVertexNorm>│
└─────────────────────┴─────────────────────┴─────────────────────┘
          │                   │                      │
          └───────────────────┼──────────────────────┘
                              ▼
                    ┌─────────────────────┐
                    │   PrimitiveBuffer   │
                    │  (GPU-ready data)   │
                    └─────────────────────┘
```

---

## State Transitions

```
MeshResource State Machine:

    ┌─────────┐  load()   ┌──────────┐  write()  ┌──────────┐
    │ Created │ ────────► │  Loaded  │ ────────► │  Written │
    └─────────┘           └──────────┘           └──────────┘
         │                     │                      │
         │ (mesh data set)     │ (BVH built,         │ (in GPU buffer,
         │                     │  normals computed)   │  ready for queries)
         ▼                     ▼                      ▼
    SpatialMeshData       m_payloadData          Primitives buffer
    populated             serialized              updated
```

---

## Validation Rules

1. **Mesh must be non-empty**: `triangles.size() > 0`
2. **BVH root at index 0**: `nodes[0]` is always the root
3. **Valid vertex indices**: All `vertexIndices[i] < vertexNormals.size()`
4. **Normalized normals**: `length(vertexNormal.normal) ≈ 1.0` (within epsilon)
5. **Consistent winding**: All triangles have consistent CCW winding for correct sign
