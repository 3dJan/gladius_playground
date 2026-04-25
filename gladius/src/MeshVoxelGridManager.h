/// @file MeshVoxelGridManager.h
/// @brief Manages GPU-built voxel acceleration grids for mesh SDF queries
/// @details Implements Option G from spec 002-mesh-sdf-performance
/// @see mesh_sdf.cl, MeshVoxelGrid.h, SpatialMeshResource.h

#pragma once

#include "ComputeContext.h"
#include "MeshVoxelGrid.h"

#include <vector>

namespace gladius
{
    class CLProgram;
    
    /// Parameters needed to build a voxel grid for a spatial mesh resource
    struct MeshVoxelGridBuildParams
    {
        int headerStart;        ///< Start offset of mesh header in primitive buffer
        int voxelDataOffset;    ///< Offset where voxel data should be written
        int nodesOffset;        ///< BVH nodes offset in primitive buffer
        int trianglesOffset;    ///< Triangles offset in primitive buffer
        int normalsOffset;      ///< Vertex normals offset in primitive buffer
        int indicesOffset;      ///< Vertex indices offset in primitive buffer
        int edgeNeighborsOffset; ///< Per-edge adjacent face normals offset in primitive buffer
        int nodeCount;          ///< Number of BVH nodes
        int triCount;           ///< Number of triangles
        int vertexNormalCount;  ///< Number of vertex normals
        int voxelCount;         ///< Total number of voxels (dims.x * dims.y * dims.z)
    };

    /// Parameters needed to build the FWN coarse sign cache for a spatial mesh
    /// resource. The cache is a resolution^3 grid of conservative two-bit cell
    /// states used by `spatialMeshSDF_FastWindingNumber` to bypass the winding
    /// traversal when the query is far enough from the surface that a coarse
    /// sign suffices.
    struct MeshSignCacheBuildParams
    {
        int headerStart;          ///< Mesh header offset in primitive buffer
        int signCacheDataOffset;  ///< Absolute offset where sign-cache words live
        int signCacheReadyOffset; ///< Header slot to flip to signCacheDataOffset on completion
        int signCacheBetaOffset;  ///< Header slot storing the beta used for this cache
        int nodesOffset;          ///< BVH nodes offset
        int trianglesOffset;      ///< Triangles offset
        int normalsOffset;        ///< Vertex normals offset
        int indicesOffset;        ///< Vertex indices offset
        int edgeNeighborsOffset;  ///< Per-edge adjacent face normals offset
        int fwnAggregatesOffset;  ///< FWN aggregates offset
        int nodeCount;            ///< Number of BVH nodes
        int triCount;             ///< Number of triangles
        int vertexNormalCount;    ///< Number of vertex normals
        int resolution;           ///< Cells per axis (currently 64)
        int wordCount;            ///< Total number of 32-bit words in the 2-bit cell-state map
        int baseWord;             ///< First 32-bit word to build in this incremental step
        int wordsToBuild;         ///< Number of words to build in this incremental step
        bool completesBuild;      ///< True when this step queues the final cache chunk and ready marker
        float fwnBeta;            ///< Barnes-Hut beta used while building this cache
    };
    
    /// Builds voxel acceleration grids on the GPU
    /// @details Runs the buildMeshVoxelGrid kernel to populate voxel grids
    ///          embedded in the primitive buffer after resource upload.
    /// @note Currently not instantiated - voxel grid building is done through
    ///       SlicerProgram::buildMeshVoxelGrid() called from ComputeCore.
    ///       This class is retained for potential future refactoring to
    ///       encapsulate voxel grid management.
    class MeshVoxelGridManager
    {
      public:
        /// Construct with compute context
        /// @param context Shared compute context for GPU operations
        explicit MeshVoxelGridManager(SharedComputeContext context);
        
        ~MeshVoxelGridManager() = default;
        
        // Non-copyable, non-movable (holds reference to context)
        MeshVoxelGridManager(MeshVoxelGridManager const&) = delete;
        MeshVoxelGridManager& operator=(MeshVoxelGridManager const&) = delete;
        MeshVoxelGridManager(MeshVoxelGridManager&&) = delete;
        MeshVoxelGridManager& operator=(MeshVoxelGridManager&&) = delete;
        
        /// Build voxel grid in-place in the primitive buffer
        /// @param program CLProgram with compiled buildMeshVoxelGrid kernel
        /// @param primitiveDataBuffer Primitive data buffer on GPU (read BVH, write voxels)
        /// @param params Build parameters (offsets and counts)
        /// @return true if the kernel was executed successfully
        bool buildGrid(CLProgram& program,
                       cl::Buffer& primitiveDataBuffer,
                       MeshVoxelGridBuildParams const& params);
        
        /// Queue a build request (call during resource iteration)
        void queueBuild(MeshVoxelGridBuildParams params);
        
        /// Execute all queued builds
        /// @param program CLProgram with compiled buildMeshVoxelGrid kernel
        /// @param primitiveDataBuffer Primitive data buffer on GPU
        void executeQueuedBuilds(CLProgram& program, cl::Buffer& primitiveDataBuffer);
        
        /// Clear the build queue
        void clearQueue();
        
        /// Get number of queued builds
        [[nodiscard]] size_t getQueuedCount() const { return m_buildQueue.size(); }
        
      private:
        SharedComputeContext m_context;
        std::vector<MeshVoxelGridBuildParams> m_buildQueue;
    };
    
}  // namespace gladius
