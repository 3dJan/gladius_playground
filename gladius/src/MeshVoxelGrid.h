/// @file MeshVoxelGrid.h
/// @brief GPU-compatible data structures for mesh SDF voxel acceleration grid
/// @details Implements Option G from spec 002-mesh-sdf-performance
/// @see mesh_sdf.cl, SpatialMeshResource.h

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace gladius
{
    // ========================================================================
    // GPU Data Structures for Voxel Acceleration Grid
    // ========================================================================

    /// Per-voxel data stored in GPU buffer
    /// Memory layout: 2 floats = 8 bytes per voxel
    /// @note Triangle index stored as float for GPU compatibility
    struct MeshVoxelData
    {
        float nearestTriIndex;   ///< Index of nearest triangle (as float for GPU)
        float approxSignedDist;  ///< Signed distance at voxel center (precomputed)
    };

    /// Voxel grid header (uploaded once, 10 floats)
    /// Contains grid metadata for GPU lookups
    /// @note All values stored as floats for consistent GPU memory access
    struct MeshVoxelGridHeader
    {
        float originX;      ///< Grid origin X (bbox min)
        float originY;      ///< Grid origin Y
        float originZ;      ///< Grid origin Z
        float dimX;         ///< Grid dimension X (as float)
        float dimY;         ///< Grid dimension Y
        float dimZ;         ///< Grid dimension Z
        float voxelSize;    ///< Size of each voxel
        float invVoxelSize; ///< 1.0 / voxelSize (precomputed for GPU)
        float threshold;    ///< Distance threshold for BVH fallback
        float padding;      ///< Padding for alignment (16-byte aligned)
    };

    /// Grid dimensions (3 integers)
    struct VoxelGridDimensions
    {
        int x;
        int y;
        int z;
    };

    // ========================================================================
    // Constants
    // ========================================================================

    /// Default voxel grid resolution (32³ = 32K voxels, 256KB)
    static constexpr int kDefaultVoxelGridResolution = 32;

    /// Maximum voxel grid resolution (64³ = 262K voxels, 2MB)
    static constexpr int kMaxVoxelGridResolution = 64;

    /// Threshold multiplier: BVH fallback when |dist| < threshold * voxelSize
    static constexpr float kVoxelThresholdMultiplier = 1.5f;

    // ========================================================================
    // Helper Functions
    // ========================================================================

    /// Compute voxel size from bounding box extent
    /// @param extentX Bounding box extent X
    /// @param extentY Bounding box extent Y
    /// @param extentZ Bounding box extent Z
    /// @param targetResolution Target resolution per axis (e.g., 32)
    /// @return Voxel size for uniform grid
    inline float computeVoxelSize(float extentX, float extentY, float extentZ,
                                   int targetResolution)
    {
        float maxExtent = std::max({extentX, extentY, extentZ});
        return maxExtent / static_cast<float>(targetResolution);
    }

    /// Compute grid dimensions from bounding box and voxel size
    /// @param extentX Bounding box extent X
    /// @param extentY Bounding box extent Y
    /// @param extentZ Bounding box extent Z
    /// @param voxelSize Size of each voxel
    /// @return Grid dimensions (x, y, z)
    inline VoxelGridDimensions computeGridDimensions(float extentX, float extentY, 
                                                      float extentZ, float voxelSize)
    {
        VoxelGridDimensions dims;
        dims.x = std::max(1, static_cast<int>(std::ceil(extentX / voxelSize)));
        dims.y = std::max(1, static_cast<int>(std::ceil(extentY / voxelSize)));
        dims.z = std::max(1, static_cast<int>(std::ceil(extentZ / voxelSize)));
        return dims;
    }

    /// Initialize voxel grid header from bounding box
    /// @param bboxMinX, bboxMinY, bboxMinZ Bounding box minimum
    /// @param bboxMaxX, bboxMaxY, bboxMaxZ Bounding box maximum
    /// @param targetResolution Target resolution per axis
    /// @return Initialized header structure
    inline MeshVoxelGridHeader createVoxelGridHeader(float bboxMinX, float bboxMinY, float bboxMinZ,
                                                      float bboxMaxX, float bboxMaxY, float bboxMaxZ,
                                                      int targetResolution = kDefaultVoxelGridResolution)
    {
        float extentX = bboxMaxX - bboxMinX;
        float extentY = bboxMaxY - bboxMinY;
        float extentZ = bboxMaxZ - bboxMinZ;
        
        float voxelSize = computeVoxelSize(extentX, extentY, extentZ, targetResolution);
        VoxelGridDimensions dims = computeGridDimensions(extentX, extentY, extentZ, voxelSize);
        
        MeshVoxelGridHeader header{};
        header.originX = bboxMinX;
        header.originY = bboxMinY;
        header.originZ = bboxMinZ;
        header.dimX = static_cast<float>(dims.x);
        header.dimY = static_cast<float>(dims.y);
        header.dimZ = static_cast<float>(dims.z);
        header.voxelSize = voxelSize;
        header.invVoxelSize = 1.0f / voxelSize;
        header.threshold = kVoxelThresholdMultiplier * voxelSize;
        header.padding = 0.0f;
        
        return header;
    }

    /// Compute total number of voxels in grid
    /// @param header Voxel grid header
    /// @return Total voxel count
    inline size_t computeVoxelCount(MeshVoxelGridHeader const& header)
    {
        return static_cast<size_t>(header.dimX) * 
               static_cast<size_t>(header.dimY) * 
               static_cast<size_t>(header.dimZ);
    }

    /// Compute GPU buffer size for voxel data
    /// @param header Voxel grid header
    /// @return Size in bytes
    inline size_t computeVoxelBufferSize(MeshVoxelGridHeader const& header)
    {
        return computeVoxelCount(header) * sizeof(MeshVoxelData);
    }

}  // namespace gladius
