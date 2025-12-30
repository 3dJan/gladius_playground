#pragma once

/// @file SpatialMeshResource.h
/// @brief Resource class for mesh geometry with BVH for SDF queries
/// @details Analogous to VdbResource but uses BVH-based spatial acceleration
///          instead of voxel grids for signed distance queries.
///
/// @see MeshBVH.h for the BVH builder
/// @see VdbResource.h for the similar pattern used with OpenVDB grids

#include "MeshBVH.h"
#include "ResourceManager.h"

#include <span>

namespace gladius
{
    /// Resource containing mesh geometry with BVH for SDF queries
    /// @details Follows the ResourceBase pattern (same as VdbResource).
    ///          Serializes BVH nodes, triangles, and vertex normals to PrimitiveBuffer
    ///          for GPU access via OpenCL kernels.
    class SpatialMeshResource : public ResourceBase
    {
      public:
        /// Construct from pre-built spatial data
        /// @param key Resource identifier
        /// @param data Pre-built BVH and mesh data (moved)
        explicit SpatialMeshResource(ResourceKey key, SpatialMeshData && data);

        /// Construct from raw mesh (will build BVH)
        /// @param key Resource identifier
        /// @param vertices Vertex positions
        /// @param indices Triangle indices (3 per triangle)
        SpatialMeshResource(ResourceKey key,
                            std::span<float4 const> vertices,
                            std::span<TriangleIndices const> indices);

        ~SpatialMeshResource() = default;

        /// Get the spatial mesh data
        SpatialMeshData const & getData() const
        {
            return m_data;
        }

        /// Get the mesh bounding box
        BoundingBox const & getBoundingBox() const
        {
            return m_data.boundingBox;
        }

        /// Get triangle count
        size_t getTriangleCount() const
        {
            return m_data.originalTriangleCount;
        }

        /// Mark resource as needing rebuild (for mesh modification support)
        void invalidate();

        /// Rebuild BVH from updated mesh data
        /// @param vertices Updated vertex positions
        /// @param indices Updated triangle indices
        void rebuild(std::span<float4 const> vertices,
                     std::span<TriangleIndices const> indices);

      protected:
        /// Serialize to PrimitiveBuffer for GPU access
        void loadImpl() override;

      private:
        SpatialMeshData m_data;
        bool m_needsRebuild = false;
    };
}
