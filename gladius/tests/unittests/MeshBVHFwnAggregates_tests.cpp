/// @file MeshBVHFwnAggregates_tests.cpp
/// @brief Unit tests for Fast-Winding-Number multipole aggregates on the BVH.
/// @see MeshBVH.h::computeFwnAggregates

#include "MeshBVH.h"
#include "MeshSdfReference.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        std::vector<float4> cubeVertices()
        {
            return {
                {-1.f, -1.f, -1.f, 0.f}, {1.f, -1.f, -1.f, 0.f},
                {1.f, 1.f, -1.f, 0.f},   {-1.f, 1.f, -1.f, 0.f},
                {-1.f, -1.f, 1.f, 0.f},  {1.f, -1.f, 1.f, 0.f},
                {1.f, 1.f, 1.f, 0.f},    {-1.f, 1.f, 1.f, 0.f},
            };
        }
        std::vector<TriangleIndices> cubeIndices()
        {
            return {
                {4, 5, 6}, {4, 6, 7}, {1, 0, 3}, {1, 3, 2}, {5, 1, 2}, {5, 2, 6},
                {0, 4, 7}, {0, 7, 3}, {7, 6, 2}, {7, 2, 3}, {0, 1, 5}, {0, 5, 4},
            };
        }
    } // namespace

    TEST(MeshBVHFwnAggregates, EmptyMesh_ReturnsEmptyAggregates)
    {
        SpatialMeshData data; // empty
        computeFwnAggregates(data);
        EXPECT_TRUE(data.fwnAggregates.empty());
    }

    TEST(MeshBVHFwnAggregates, Cube_RootHasCorrectArea)
    {
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(cubeVertices(), cubeIndices());
        computeFwnAggregates(data);

        ASSERT_FALSE(data.fwnAggregates.empty());
        ASSERT_EQ(data.fwnAggregates.size(), data.nodes.size());

        // Total surface area of a cube of side 2 is 6 × 4 = 24.
        EXPECT_NEAR(data.fwnAggregates[0].areaCentroid.w, 24.f, 1e-3f);
    }

    TEST(MeshBVHFwnAggregates, Cube_RootCentroidIsOrigin)
    {
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(cubeVertices(), cubeIndices());
        computeFwnAggregates(data);

        ASSERT_FALSE(data.fwnAggregates.empty());
        auto const & root = data.fwnAggregates[0];
        ASSERT_GT(root.areaCentroid.w, 0.f);
        EXPECT_NEAR(root.areaCentroid.x / root.areaCentroid.w, 0.f, 1e-4f);
        EXPECT_NEAR(root.areaCentroid.y / root.areaCentroid.w, 0.f, 1e-4f);
        EXPECT_NEAR(root.areaCentroid.z / root.areaCentroid.w, 0.f, 1e-4f);
    }

    TEST(MeshBVHFwnAggregates, Cube_WeightedNormalSumIsZero)
    {
        // For a closed mesh, Σ(2·area·n) cancels out to ~0 because face normals
        // on opposite sides cancel.
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(cubeVertices(), cubeIndices());
        computeFwnAggregates(data);

        ASSERT_FALSE(data.fwnAggregates.empty());
        auto const & root = data.fwnAggregates[0];
        EXPECT_NEAR(root.weightedNormalSum.x, 0.f, 1e-3f);
        EXPECT_NEAR(root.weightedNormalSum.y, 0.f, 1e-3f);
        EXPECT_NEAR(root.weightedNormalSum.z, 0.f, 1e-3f);
    }

    TEST(MeshBVHFwnAggregates, Cube_RadiusEnclosesAllVertices)
    {
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(cubeVertices(), cubeIndices());
        computeFwnAggregates(data);

        ASSERT_FALSE(data.fwnAggregates.empty());
        auto const & root = data.fwnAggregates[0];
        // Root centroid is origin, farthest vertex is at sqrt(3) ≈ 1.732.
        EXPECT_GE(root.weightedNormalSum.w, std::sqrt(3.f) - 1e-4f);
    }

    TEST(MeshBVHFwnAggregates, Cube_LeafSumsEqualRoot)
    {
        // Σ over leaves of weightedNormalSum.xyz and areaCentroid should equal root.
        MeshBVHBuilder builder;
        SpatialMeshData data = builder.build(cubeVertices(), cubeIndices());
        computeFwnAggregates(data);

        float wnX = 0.f, wnY = 0.f, wnZ = 0.f;
        float acX = 0.f, acY = 0.f, acZ = 0.f, acW = 0.f;
        for (std::size_t i = 0; i < data.nodes.size(); ++i)
        {
            if (!data.nodes[i].isLeaf())
            {
                continue;
            }
            auto const & ag = data.fwnAggregates[i];
            wnX += ag.weightedNormalSum.x;
            wnY += ag.weightedNormalSum.y;
            wnZ += ag.weightedNormalSum.z;
            acX += ag.areaCentroid.x;
            acY += ag.areaCentroid.y;
            acZ += ag.areaCentroid.z;
            acW += ag.areaCentroid.w;
        }
        auto const & root = data.fwnAggregates[0];
        EXPECT_NEAR(root.weightedNormalSum.x, wnX, 1e-3f);
        EXPECT_NEAR(root.weightedNormalSum.y, wnY, 1e-3f);
        EXPECT_NEAR(root.weightedNormalSum.z, wnZ, 1e-3f);
        EXPECT_NEAR(root.areaCentroid.x, acX, 1e-3f);
        EXPECT_NEAR(root.areaCentroid.y, acY, 1e-3f);
        EXPECT_NEAR(root.areaCentroid.z, acZ, 1e-3f);
        EXPECT_NEAR(root.areaCentroid.w, acW, 1e-3f);
    }

} // namespace gladius::tests
