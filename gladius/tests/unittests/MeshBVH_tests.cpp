/// @file MeshBVH_tests.cpp
/// @brief Unit tests for MeshBVHBuilder
/// @see MeshBVH.h

#include "MeshBVH.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace gladius::tests
{
    // ========================================================================
    // Test Fixtures
    // ========================================================================

    class MeshBVHBuilder_Test : public ::testing::Test
    {
      protected:
        MeshBVHBuilder builder;

        // Helper to create a simple cube mesh (8 vertices, 12 triangles)
        static void createCubeMesh(std::vector<float4> & vertices,
                                   std::vector<TriangleIndices> & indices)
        {
            // Cube vertices (unit cube centered at origin)
            vertices = {
                {-0.5f, -0.5f, -0.5f, 0.f},  // 0
                { 0.5f, -0.5f, -0.5f, 0.f},  // 1
                { 0.5f,  0.5f, -0.5f, 0.f},  // 2
                {-0.5f,  0.5f, -0.5f, 0.f},  // 3
                {-0.5f, -0.5f,  0.5f, 0.f},  // 4
                { 0.5f, -0.5f,  0.5f, 0.f},  // 5
                { 0.5f,  0.5f,  0.5f, 0.f},  // 6
                {-0.5f,  0.5f,  0.5f, 0.f},  // 7
            };

            // 12 triangles (2 per face, CCW winding)
            indices = {
                // Front face (z = 0.5)
                {4, 5, 6}, {4, 6, 7},
                // Back face (z = -0.5)
                {1, 0, 3}, {1, 3, 2},
                // Right face (x = 0.5)
                {5, 1, 2}, {5, 2, 6},
                // Left face (x = -0.5)
                {0, 4, 7}, {0, 7, 3},
                // Top face (y = 0.5)
                {7, 6, 2}, {7, 2, 3},
                // Bottom face (y = -0.5)
                {0, 1, 5}, {0, 5, 4},
            };
        }

        // Helper to create a single triangle
        static void createSingleTriangle(std::vector<float4> & vertices,
                                         std::vector<TriangleIndices> & indices)
        {
            vertices = {
                {0.f, 0.f, 0.f, 0.f},
                {1.f, 0.f, 0.f, 0.f},
                {0.f, 1.f, 0.f, 0.f},
            };
            indices = {{0, 1, 2}};
        }
    };

    // ========================================================================
    // T015: Empty mesh test
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_EmptyMesh_ReturnsEmptyData)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;

        auto result = builder.build(vertices, indices);

        EXPECT_TRUE(result.empty());
        EXPECT_EQ(result.nodes.size(), 0u);
        EXPECT_EQ(result.triangles.size(), 0u);
        EXPECT_EQ(result.vertexNormals.size(), 0u);
    }

    // ========================================================================
    // T016: Single triangle test
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_SingleTriangle_ProducesValidBVH)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createSingleTriangle(vertices, indices);

        auto result = builder.build(vertices, indices);

        EXPECT_FALSE(result.empty());
        EXPECT_EQ(result.triangles.size(), 1u);
        EXPECT_GE(result.nodes.size(), 1u);
        EXPECT_EQ(result.vertexNormals.size(), 3u);

        // Root node should be a leaf with 1 primitive
        EXPECT_TRUE(result.nodes[0].isLeaf());
        EXPECT_EQ(result.nodes[0].primCount, 1);

        // Stats should reflect single triangle
        auto const & stats = builder.getLastBuildStats();
        EXPECT_EQ(stats.totalNodes, 1);
        EXPECT_EQ(stats.leafNodes, 1);
    }

    // ========================================================================
    // T017: Cube mesh test
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_Cube_ProducesValidBVH)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        auto result = builder.build(vertices, indices);

        EXPECT_FALSE(result.empty());
        EXPECT_EQ(result.triangles.size(), 12u);
        EXPECT_EQ(result.originalTriangleCount, 12u);
        EXPECT_EQ(result.vertexNormals.size(), 8u);

        // Root node at index 0
        EXPECT_GE(result.nodes.size(), 1u);

        // BVH should have reasonable depth
        auto const & stats = builder.getLastBuildStats();
        EXPECT_GT(stats.totalNodes, 0);
        EXPECT_LE(stats.maxDepth, 24);  // Within max depth limit
        EXPECT_GT(stats.avgPrimitivesPerLeaf, 0.0f);
        EXPECT_LE(stats.avgPrimitivesPerLeaf, 12.0f);

        // Bounding box should encompass the unit cube
        EXPECT_LE(result.boundingBox.min.x, -0.5f);
        EXPECT_LE(result.boundingBox.min.y, -0.5f);
        EXPECT_LE(result.boundingBox.min.z, -0.5f);
        EXPECT_GE(result.boundingBox.max.x, 0.5f);
        EXPECT_GE(result.boundingBox.max.y, 0.5f);
        EXPECT_GE(result.boundingBox.max.z, 0.5f);
    }

    // ========================================================================
    // T018: Angle-weighted normals test
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_ComputesAngleWeightedNormals)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        auto result = builder.build(vertices, indices);

        EXPECT_EQ(result.vertexNormals.size(), 8u);

        // Each corner vertex of a cube has 3 incident faces at 90° angles
        // The angle-weighted normal should point outward from the corner
        for (size_t i = 0; i < result.vertexNormals.size(); ++i)
        {
            auto const & n = result.vertexNormals[i].normal;
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);

            // Normals should be normalized (or close to it)
            EXPECT_NEAR(len, 1.0f, 0.01f) << "Vertex normal " << i << " not normalized";

            // For a cube corner at (+/-0.5, +/-0.5, +/-0.5), the normal should
            // point in the same general direction as the position
            float4 pos = vertices[i];
            float dotProduct = n.x * pos.x + n.y * pos.y + n.z * pos.z;

            // Dot product should be positive (normal points outward)
            EXPECT_GT(dotProduct, 0.0f) 
                << "Vertex " << i << " normal does not point outward";
        }
    }

    // ========================================================================
    // Edge-neighbour face normals: cube is closed and 2-manifold, so every
    // triangle edge must have a present (w=1) unit-length adjacent normal.
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_Cube_EdgeNeighborNormals_AllPresentAndUnit)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        auto result = builder.build(vertices, indices);

        ASSERT_EQ(result.edgeNeighborNormals.size(), result.triangles.size() * 3u);

        for (size_t i = 0; i < result.edgeNeighborNormals.size(); ++i)
        {
            auto const & n = result.edgeNeighborNormals[i].normal;
            EXPECT_NEAR(n.w, 1.0f, 1e-5f)
                << "Edge slot " << i << " marked missing on closed cube";
            float const len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            EXPECT_NEAR(len, 1.0f, 1e-4f)
                << "Edge slot " << i << " normal not unit length";
        }
    }

    // ========================================================================
    // Edge-neighbour face normals on a 2-triangle crease: exactly one shared
    // edge per triangle, the rest are boundary (w=0). The shared-edge entry
    // must hold the *other* triangle's unit face normal.
    // ========================================================================

    TEST_F(MeshBVHBuilder_Test, Build_Crease_EdgeNeighborNormals_SharedEdgeOnly)
    {
        // Two triangles meeting at a 90° crease along the x-axis.
        std::vector<float4> const vertices = {
            {0.0f, 0.0f, 0.0f, 0.f},
            {1.0f, 0.0f, 0.0f, 0.f},
            {0.5f, 1.0f, 0.0f, 0.f},
            {0.5f, 0.0f, 1.0f, 0.f},
        };
        std::vector<TriangleIndices> const indices = {
            {0, 1, 2},  // XY plane,  face normal = (0, 0, +1)
            {1, 0, 3},  // XZ plane,  face normal = (0, +1, 0)
        };

        auto result = builder.build(vertices, indices);

        ASSERT_EQ(result.triangles.size(), 2u);
        ASSERT_EQ(result.edgeNeighborNormals.size(), 6u);

        int presentCount = 0;
        for (auto const & en : result.edgeNeighborNormals)
        {
            presentCount += (en.normal.w > 0.5f) ? 1 : 0;
        }
        // Exactly one shared edge → contributes one entry per side = 2 present.
        EXPECT_EQ(presentCount, 2);

        // The two present neighbour normals must be unit vectors, and their
        // sum (per Bærentzen-Aanæs edge pseudo-normal) must bisect the dihedral
        // — i.e. point along (0, 1, 1)/√2.
        for (auto const & en : result.edgeNeighborNormals)
        {
            if (en.normal.w > 0.5f)
            {
                float const len = std::sqrt(en.normal.x * en.normal.x +
                                            en.normal.y * en.normal.y +
                                            en.normal.z * en.normal.z);
                EXPECT_NEAR(len, 1.0f, 1e-4f);
            }
        }
    }

}  // namespace gladius::tests
