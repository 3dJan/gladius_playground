#pragma once

/// @file SpatialMeshResource.h
/// @brief Resource class for mesh geometry with BVH for SDF queries
/// @details Analogous to VdbResource but uses BVH-based spatial acceleration
///          instead of voxel grids for signed distance queries.
///
/// @see MeshBVH.h for the BVH builder
/// @see VdbResource.h for the similar pattern used with OpenVDB grids
/// @see MeshResourceBase.h for the common mesh resource interface

#include "MeshBVH.h"
#include "MeshResourceBase.h"
#include "MeshSdfMethod.h"
#include "MeshVoxelGridManager.h"

#include <span>

namespace gladius
{
    /// Resource containing mesh geometry with BVH for SDF queries
    /// @details Follows the ResourceBase pattern (same as VdbResource).
    ///          Serializes BVH nodes, triangles, and vertex normals to PrimitiveBuffer
    ///          for GPU access via OpenCL kernels.
    class SpatialMeshResource : public MeshResourceBase
    {
      public:
        /// Construct from pre-built spatial data
        /// @param key Resource identifier
        /// @param data Pre-built BVH and mesh data (moved)
        explicit SpatialMeshResource(ResourceKey key, SpatialMeshData && data);

        /// Construct from pre-built spatial data with an explicit SDF evaluation configuration.
        /// @param key Resource identifier
        /// @param data Pre-built BVH and mesh data (moved)
        /// @param evaluationConfig Mesh SDF evaluation configuration used while serializing payload data
        SpatialMeshResource(ResourceKey key,
                            SpatialMeshData && data,
                            MeshSdfEvaluationConfig const & evaluationConfig);

        /// Construct from raw mesh (will build BVH)
        /// @param key Resource identifier
        /// @param vertices Vertex positions
        /// @param indices Triangle indices (3 per triangle)
        SpatialMeshResource(ResourceKey key,
                            std::span<float4 const> vertices,
                            std::span<TriangleIndices const> indices);

        ~SpatialMeshResource() = default;

        /// Get the spatial mesh data
        [[nodiscard]] SpatialMeshData const & getData() const
        {
            return m_data;
        }

        // MeshResourceBase interface
        [[nodiscard]] BoundingBox getBoundingBox() const override
        {
            return m_data.boundingBox;
        }

        [[nodiscard]] size_t getTriangleCount() const override
        {
            return m_data.originalTriangleCount;
        }

        [[nodiscard]] std::string getMeshTypeName() const override
        {
            return "SpatialMesh (BVH)";
        }

        /// Get the mesh bounding box (deprecated - use getBoundingBox())
        [[deprecated("Use getBoundingBox() instead")]]
        BoundingBox const & getBoundingBox_DEPRECATED() const
        {
            return m_data.boundingBox;
        }

        /// Get triangle count (deprecated - use getTriangleCount())
        [[deprecated("Use getTriangleCount() instead")]]
        size_t getTriangleCount_DEPRECATED() const
        {
            return m_data.originalTriangleCount;
        }

        /// Mark resource as needing rebuild (for mesh modification support)
        void invalidate();

        /// Apply a new SDF-evaluation configuration. If @p cfg requires a
        /// different acceleration structure (different method or voxel grid
        /// resolution) this call invalidates the resource so the next load
        /// rebuilds the payload. Pure runtime knobs (inflation, early-exit
        /// toggle) are stored elsewhere and do not trigger a rebuild here.
        /// @return true if the resource was invalidated.
        bool setEvaluationConfig(MeshSdfEvaluationConfig const & cfg);

        /// Currently-applied evaluation configuration.
        [[nodiscard]] MeshSdfEvaluationConfig const & evaluationConfig() const noexcept
        {
            return m_evaluationConfig;
        }

        /// Rebuild BVH from updated mesh data
        /// @param vertices Updated vertex positions
        /// @param indices Updated triangle indices
        void rebuild(std::span<float4 const> vertices,
                     std::span<TriangleIndices const> indices);
        
        /// Override write to track base offset for voxel grid build
        void write(Primitives & primitives) override;
        
        /// Get voxel grid build parameters (valid after write)
        /// @return Build parameters with adjusted offsets, or nullopt if not ready
        [[nodiscard]] std::optional<MeshVoxelGridBuildParams> getVoxelGridBuildParams() const;

        /// Get FWN aggregate build parameters (valid after write)
        /// @return Build parameters with adjusted offsets, or nullopt if not ready
        [[nodiscard]] std::optional<MeshFwnAggregateBuildParams> getFwnAggregateBuildParams() const;

        /// Check if FWN aggregate build is needed
        [[nodiscard]] bool needsFwnAggregateBuild() const { return m_needsFwnAggregateBuild; }

        /// Mark FWN aggregates as built in the GPU primitive buffer
        void markFwnAggregatesBuilt() { m_needsFwnAggregateBuild = false; }
        
        /// Check if voxel grid build is needed
        [[nodiscard]] bool needsVoxelGridBuild() const { return m_needsVoxelGridBuild; }
        
        /// Mark voxel grid as built
        void markVoxelGridBuilt() { m_needsVoxelGridBuild = false; }

        /// Get FWN sign-cache build parameters (valid after write)
        /// @return Build parameters with adjusted offsets, or nullopt if not ready
        [[nodiscard]] std::optional<MeshSignCacheBuildParams> getSignCacheBuildParams() const;

        /// Check if FWN sign-cache build is needed
        [[nodiscard]] bool needsSignCacheBuild() const { return m_needsSignCacheBuild; }

        /// Check whether the current evaluation method uses Fast Winding Number.
        [[nodiscard]] bool usesFastWindingNumber() const noexcept
        {
            return m_evaluationConfig.method == MeshSdfMethod::FastWindingNumber;
        }

        /// Check whether the current evaluation method can consume the FWN sign cache.
        [[nodiscard]] bool usesFwnSignCache() const noexcept
        {
            return usesFastWindingNumber() && m_evaluationConfig.fwnUseSignCache;
        }

        /// Advance FWN sign-cache build progress after a build step was queued.
        /// The final step queues markMeshSignCacheReady, which patches
        /// @ref signCacheReadyHostOffset() on the GPU buffer once the bitmap is
        /// fully populated.
        void markSignCacheBuildQueued(MeshSignCacheBuildParams const & params);

        /// Absolute primitive-data index of the header slot that holds the
        /// sign-cache data offset. Writing the absolute sign-cache data offset
        /// to this slot in the GPU primitive buffer flips the cache from
        /// "not ready" (0) to "ready" for the kernel.
        [[nodiscard]] int signCacheReadyHostOffset() const;

        /// Absolute primitive-data index of the header slot storing the FWN beta
        /// used to produce the currently ready sign cache.
        [[nodiscard]] int signCacheBetaHostOffset() const;

      protected:
        /// Serialize to PrimitiveBuffer for GPU access
        void loadImpl() override;

      private:
        SpatialMeshData m_data;
        bool m_needsRebuild = false;
        
        /// Voxel grid tracking
        /// @note Thread safety: Access to m_needsVoxelGridBuild is serialized by
        ///       m_computeMutex in ComputeCore when called during document refresh.
        ///       The flag is only modified during single-threaded resource loading
        ///       (loadImpl, write) or under the compute mutex (markVoxelGridBuilt).
        bool m_needsVoxelGridBuild = true;
        int m_dataBaseOffset = 0;  ///< Base offset in primitives.data when written
        
        /// Cached local offsets (relative to m_payloadData.data start)
        size_t m_headerStart = 0;
        size_t m_nodesOffset = 0;
        size_t m_trianglesOffset = 0;
        size_t m_normalsOffset = 0;
        size_t m_indicesOffset = 0;
        size_t m_edgeNeighborsOffset = 0;
        size_t m_fwnAggregatesOffset = 0;
        size_t m_voxelDataOffset = 0;
        size_t m_voxelCount = 0;
        size_t m_signCacheDataOffset = 0;
        size_t m_nanovdbGridOffset = 0;  ///< Local float offset of the NanoVDB grid in m_payloadData (0 = not built)
        bool m_needsFwnAggregateBuild = false;
        bool m_needsSignCacheBuild = true;
        int m_signCacheNextWord = 0;

        void resetSignCacheBuildProgress() noexcept;

        /// Active evaluation configuration. Determines whether a voxel grid is
        /// allocated during loadImpl() and at what resolution.
        MeshSdfEvaluationConfig m_evaluationConfig{};
    };
}
