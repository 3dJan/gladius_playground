# Internal API Contracts: Spatial Tree Mesh SDF

**Feature**: 001-spatial-sdf  
**Date**: 2025-12-29

## 1. MeshBVHBuilder Class

**File**: `gladius/src/MeshBVH.h`

```cpp
namespace gladius
{
    /// Configuration parameters for BVH construction
    struct MeshBVHBuildParams
    {
        int maxDepth = 24;              ///< Maximum tree depth
        int maxPrimitivesPerLeaf = 4;   ///< Target primitives per leaf
        float traversalCost = 1.0f;     ///< SAH traversal cost
        float intersectionCost = 1.5f;  ///< SAH intersection cost
    };

    /// Statistics from BVH construction
    struct MeshBVHBuildStats
    {
        int totalNodes = 0;
        int leafNodes = 0;
        int maxDepth = 0;
        float avgPrimitivesPerLeaf = 0.0f;
        double buildTimeMs = 0.0;
    };

    /// Builds a BVH for triangle mesh closest-point queries
    class MeshBVHBuilder
    {
      public:
        /// Build BVH from mesh data
        /// @param vertices Vertex positions
        /// @param indices Triangle indices (3 per triangle)
        /// @param params Build configuration
        /// @return SpatialMeshData containing BVH, triangles, and normals
        SpatialMeshData build(
            std::span<Vector3 const> vertices,
            std::span<Vector3i const> indices,
            MeshBVHBuildParams const & params = {});

        /// Get statistics from last build
        MeshBVHBuildStats const & getLastBuildStats() const;

      private:
        MeshBVHBuildStats m_lastStats;
        // ... implementation details
    };
}
```

**Contract**:
- `build()` MUST produce a valid BVH with root at index 0
- `build()` MUST compute angle-weighted vertex normals
- `build()` MUST NOT throw for empty input (returns empty SpatialMeshData)
- `build()` MUST complete in O(n log n) time for n triangles

---

## 2. SpatialMeshResource Class

**File**: `gladius/src/SpatialMeshResource.h`

```cpp
namespace gladius
{
    /// Resource containing mesh geometry with BVH for SDF queries
    class SpatialMeshResource : public ResourceBase
    {
      public:
        /// Construct from pre-built spatial data
        explicit SpatialMeshResource(ResourceKey key, SpatialMeshData && data);

        /// Construct from raw mesh (will build BVH)
        SpatialMeshResource(ResourceKey key, 
                           std::span<Vector3 const> vertices,
                           std::span<Vector3i const> indices);

        /// Get the spatial mesh data
        SpatialMeshData const & getData() const;

        /// Get the mesh bounding box
        BoundingBox const & getBoundingBox() const;

        /// Get triangle count
        size_t getTriangleCount() const;

      protected:
        void loadImpl() override;

      private:
        SpatialMeshData m_data;
    };
}
```

**Contract**:
- Constructor MUST build BVH if given raw mesh data
- `loadImpl()` MUST serialize to `m_payloadData` for GPU access
- `write()` (inherited) MUST produce valid `PrimitiveBuffer` entries
- Memory layout MUST match GPU kernel expectations

---

## 3. ResourceManager Extensions

**File**: `gladius/src/ResourceManager.h` (additions)

```cpp
namespace gladius
{
    class ResourceManager
    {
      public:
        // ... existing methods ...

        /// Add a spatial mesh resource from pre-built data
        void addResource(ResourceKey key, SpatialMeshData && data);

        /// Add a spatial mesh resource from raw mesh
        void addResource(ResourceKey key,
                        std::span<Vector3 const> vertices,
                        std::span<Vector3i const> indices);
    };
}
```

**Contract**:
- `addResource()` MUST create `SpatialMeshResource` and store in `m_resources`
- `addResource()` MUST NOT duplicate resources with same key
- Resource MUST be accessible via `getResource(key)` after addition

---

## 4. OpenCL Kernel Functions

**File**: `gladius/src/kernel/mesh_sdf.cl`

```c
/// Query signed distance to mesh using BVH
/// @param pos Query point in world coordinates
/// @param rootIndex Index of SDF_SPATIAL_MESH_ROOT in primitives
/// @param primitives Global primitive metadata array
/// @param data Global primitive data array
/// @return Signed distance (negative inside, positive outside)
float spatialMeshSDF(float3 pos, 
                     int rootIndex,
                     __global struct PrimitiveMeta * primitives,
                     __global float * data);

/// Query unsigned distance to mesh using BVH
/// @param pos Query point in world coordinates
/// @param rootIndex Index of SDF_SPATIAL_MESH_ROOT in primitives
/// @param primitives Global primitive metadata array
/// @param data Global primitive data array
/// @return Unsigned (absolute) distance to mesh surface
float spatialMeshUnsignedDistance(float3 pos,
                                  int rootIndex,
                                  __global struct PrimitiveMeta * primitives,
                                  __global float * data);
```

**Contract**:
- `spatialMeshSDF()` MUST return correct sign for watertight meshes
- `spatialMeshUnsignedDistance()` MUST work for any mesh (open or closed)
- Both MUST handle empty meshes (return `FLT_MAX`)
- Both MUST NOT access out-of-bounds memory
- Performance MUST be O(log n) average case for n triangles

---

## 5. PrimitiveType Additions

**File**: `gladius/src/kernel/types.h` (additions)

```cpp
enum PrimitiveType
{
    // ... existing values (0-19) ...
    
    SDF_SPATIAL_MESH_ROOT = 20,    ///< Root metadata for spatial mesh SDF
    SDF_SPATIAL_MESH_NODES = 21,   ///< BVH node array
    SDF_SPATIAL_MESH_TRIS = 22,    ///< Triangle data
    SDF_SPATIAL_MESH_NORMALS = 23, ///< Weighted vertex normals
};
```

**Contract**:
- Values MUST NOT conflict with existing `PrimitiveType` values
- Values MUST be contiguous for this feature

---

## 6. Node Integration

**File**: `gladius/src/nodes/DerivedNodes.h` (modifications)

The `SignedDistanceToMesh` node class needs to support both backends:

```cpp
// In mesh node evaluation (pseudo-code)
if (useSpatialBackend && hasSpatialMeshResource(meshId))
{
    // Use spatial mesh SDF path
    return spatialMeshSDF(pos, resourceIndex, PASS_PAYLOAD_ARGS);
}
else if (hasVdbResource(meshId))
{
    // Existing NanoVDB path
    return vdbModel(pos, resourceIndex, PASS_PAYLOAD_ARGS);
}
```

**Contract**:
- Spatial path MUST be preferred when available
- Fallback to VDB path MUST be transparent to user
- Node MUST work with existing 3MF files without modification
