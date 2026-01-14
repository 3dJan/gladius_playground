/// @file MeshVoxelGrid_tests.cpp
/// @brief Tests for mesh voxel acceleration grid data structures
/// @details Tests for spec 002-mesh-sdf-performance Option G

#include "MeshVoxelGrid.h"

#include <gtest/gtest.h>

#include <cmath>

namespace gladius::tests
{
    // ========================================================================
    // MeshVoxelGrid Data Structure Tests
    // ========================================================================

    TEST(MeshVoxelGridTest, VoxelData_HasCorrectSize)
    {
        // MeshVoxelData should be 8 bytes (2 floats)
        EXPECT_EQ(sizeof(MeshVoxelData), 8u);
    }

    TEST(MeshVoxelGridTest, VoxelGridHeader_HasCorrectSize)
    {
        // MeshVoxelGridHeader should be 40 bytes (10 floats)
        EXPECT_EQ(sizeof(MeshVoxelGridHeader), 40u);
    }

    TEST(MeshVoxelGridTest, ComputeVoxelSize_UniformCube)
    {
        // Unit cube should have voxelSize = 1/32 at resolution 32
        float voxelSize = computeVoxelSize(1.0f, 1.0f, 1.0f, 32);
        EXPECT_FLOAT_EQ(voxelSize, 1.0f / 32.0f);
    }

    TEST(MeshVoxelGridTest, ComputeVoxelSize_NonUniformBox)
    {
        // Box 2x1x1 should have voxelSize = 2/32 at resolution 32 (based on max extent)
        float voxelSize = computeVoxelSize(2.0f, 1.0f, 1.0f, 32);
        EXPECT_FLOAT_EQ(voxelSize, 2.0f / 32.0f);
    }

    TEST(MeshVoxelGridTest, ComputeGridDimensions_UniformCube)
    {
        // Unit cube at voxelSize 0.1 should have ~10x10x10 dimensions
        VoxelGridDimensions dims = computeGridDimensions(1.0f, 1.0f, 1.0f, 0.1f);
        EXPECT_EQ(dims.x, 10);
        EXPECT_EQ(dims.y, 10);
        EXPECT_EQ(dims.z, 10);
    }

    TEST(MeshVoxelGridTest, ComputeGridDimensions_RoundsUp)
    {
        // 0.95 / 0.1 = 9.5, should round up to 10
        VoxelGridDimensions dims = computeGridDimensions(0.95f, 0.95f, 0.95f, 0.1f);
        EXPECT_EQ(dims.x, 10);
        EXPECT_EQ(dims.y, 10);
        EXPECT_EQ(dims.z, 10);
    }

    TEST(MeshVoxelGridTest, ComputeGridDimensions_MinimumOne)
    {
        // Very small extent should still have at least 1 voxel
        VoxelGridDimensions dims = computeGridDimensions(0.001f, 0.001f, 0.001f, 1.0f);
        EXPECT_GE(dims.x, 1);
        EXPECT_GE(dims.y, 1);
        EXPECT_GE(dims.z, 1);
    }

    TEST(MeshVoxelGridTest, CreateVoxelGridHeader_UnitCube)
    {
        MeshVoxelGridHeader header = createVoxelGridHeader(0.0f, 0.0f, 0.0f,
                                                           1.0f, 1.0f, 1.0f,
                                                           kDefaultVoxelGridResolution);
        
        // Origin should be bbox min
        EXPECT_FLOAT_EQ(header.originX, 0.0f);
        EXPECT_FLOAT_EQ(header.originY, 0.0f);
        EXPECT_FLOAT_EQ(header.originZ, 0.0f);
        
        // Dimensions should be 32 (default resolution)
        EXPECT_FLOAT_EQ(header.dimX, 32.0f);
        EXPECT_FLOAT_EQ(header.dimY, 32.0f);
        EXPECT_FLOAT_EQ(header.dimZ, 32.0f);
        
        // Voxel size should be 1/32
        EXPECT_FLOAT_EQ(header.voxelSize, 1.0f / 32.0f);
        EXPECT_FLOAT_EQ(header.invVoxelSize, 32.0f);
        
        // Threshold should be voxelSize * multiplier
        EXPECT_FLOAT_EQ(header.threshold, kVoxelThresholdMultiplier / 32.0f);
    }

    TEST(MeshVoxelGridTest, CreateVoxelGridHeader_OffsetOrigin)
    {
        MeshVoxelGridHeader header = createVoxelGridHeader(-5.0f, -5.0f, -5.0f,
                                                           5.0f, 5.0f, 5.0f,
                                                           32);
        
        // Origin should be bbox min
        EXPECT_FLOAT_EQ(header.originX, -5.0f);
        EXPECT_FLOAT_EQ(header.originY, -5.0f);
        EXPECT_FLOAT_EQ(header.originZ, -5.0f);
        
        // Voxel size should be 10/32 (extent is 10)
        EXPECT_FLOAT_EQ(header.voxelSize, 10.0f / 32.0f);
    }

    TEST(MeshVoxelGridTest, ComputeVoxelCount_DefaultResolution)
    {
        MeshVoxelGridHeader header = createVoxelGridHeader(0.0f, 0.0f, 0.0f,
                                                           1.0f, 1.0f, 1.0f,
                                                           32);
        
        size_t count = computeVoxelCount(header);
        EXPECT_EQ(count, 32u * 32u * 32u);  // 32768 voxels
    }

    TEST(MeshVoxelGridTest, ComputeVoxelBufferSize_DefaultResolution)
    {
        MeshVoxelGridHeader header = createVoxelGridHeader(0.0f, 0.0f, 0.0f,
                                                           1.0f, 1.0f, 1.0f,
                                                           32);
        
        size_t bufferSize = computeVoxelBufferSize(header);
        EXPECT_EQ(bufferSize, 32u * 32u * 32u * sizeof(MeshVoxelData));  // 256KB
        EXPECT_EQ(bufferSize, 262144u);  // 256 * 1024 bytes
    }

    TEST(MeshVoxelGridTest, ComputeVoxelBufferSize_MaxResolution)
    {
        MeshVoxelGridHeader header = createVoxelGridHeader(0.0f, 0.0f, 0.0f,
                                                           1.0f, 1.0f, 1.0f,
                                                           kMaxVoxelGridResolution);
        
        size_t bufferSize = computeVoxelBufferSize(header);
        EXPECT_EQ(bufferSize, 64u * 64u * 64u * sizeof(MeshVoxelData));  // 2MB
        EXPECT_EQ(bufferSize, 2097152u);  // 2 * 1024 * 1024 bytes
    }

}  // namespace gladius::tests
