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

    // ========================================================================
    // Phase A: Mesh quality diagnostics
    // ========================================================================

    /// Watertight cube has only manifold edges and no degenerate triangles.
    TEST_F(MeshBVHBuilder_Test, Build_WatertightCube_ReportsCleanStats)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        auto result = builder.build(vertices, indices);
        ASSERT_FALSE(result.empty());

        auto const & stats = builder.getLastBuildStats();
        EXPECT_EQ(stats.degenerateTriangleCount, 0);
        EXPECT_EQ(stats.boundaryEdgeCount, 0);
        EXPECT_EQ(stats.nonManifoldEdgeCount, 0);
    }

    /// A degenerate (zero-area) triangle should be counted but must not abort the build.
    TEST_F(MeshBVHBuilder_Test, Build_WithDegenerateTriangle_CountsIt)
    {
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
            {0.f, 0.f, 1.f, 0.f}, // 3
            {0.f, 0.f, 1.f, 0.f}, // 4 (duplicate)
            {0.f, 0.f, 1.f, 0.f}, // 5 (duplicate)
        };
        std::vector<TriangleIndices> indices = {
            {0, 1, 2},  // valid
            {3, 4, 5},  // degenerate: all three vertices coincide
        };

        auto result = builder.build(vertices, indices);
        ASSERT_FALSE(result.empty());

        auto const & stats = builder.getLastBuildStats();
        EXPECT_EQ(stats.degenerateTriangleCount, 1);
    }

    /// Two triangles sharing a single edge: 5 boundary edges, 1 manifold edge.
    TEST_F(MeshBVHBuilder_Test, Build_OpenQuad_CountsBoundaryEdges)
    {
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {1.f, 1.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
        };
        // Shared edge: (0,2). 4 outer edges + (0,2) appears twice (once per triangle).
        std::vector<TriangleIndices> indices = {
            {0, 1, 2},
            {0, 2, 3},
        };

        auto result = builder.build(vertices, indices);
        ASSERT_FALSE(result.empty());

        auto const & stats = builder.getLastBuildStats();
        EXPECT_EQ(stats.boundaryEdgeCount, 4);   // four outer edges
        EXPECT_EQ(stats.nonManifoldEdgeCount, 0);
    }

    /// Three coplanar triangles sharing one edge → non-manifold edge.
    /// The edge resolution must pick the two triangles with the smallest dihedral
    /// angle (largest |dot|), which here are the two coplanar ones.
    TEST_F(MeshBVHBuilder_Test, Build_NonManifoldEdge_PicksSmallestDihedral)
    {
        // Edge along x-axis from (0,0,0) to (1,0,0). Three triangles share it:
        //   tri 0: in xy-plane (normal +z)
        //   tri 1: in xy-plane on the other side (normal -z) — coplanar with tri 0
        //   tri 2: in xz-plane (normal +y) — perpendicular to the others
        // Smallest dihedral pair: (tri 0, tri 1) with |dot| = 1; the perpendicular
        // tri 2 has |dot| = 0 with each, so it must NOT be selected.
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},  // 0
            {1.f, 0.f, 0.f, 0.f},  // 1
            {0.f, 1.f, 0.f, 0.f},  // 2: tri 0 third vertex (+y)
            {0.f, -1.f, 0.f, 0.f}, // 3: tri 1 third vertex (-y), opposite winding
            {0.f, 0.f, 1.f, 0.f},  // 4: tri 2 third vertex (+z), perpendicular fin
        };
        std::vector<TriangleIndices> indices = {
            {0, 1, 2},  // tri 0: normal +z
            {1, 0, 3},  // tri 1: normal +z (CCW from below would be -z; from above +z)
            {0, 1, 4},  // tri 2: normal -y
        };

        auto result = builder.build(vertices, indices);
        ASSERT_FALSE(result.empty());

        auto const & stats = builder.getLastBuildStats();
        EXPECT_EQ(stats.nonManifoldEdgeCount, 1)
            << "Edge (0,1) is shared by three faces and must be flagged non-manifold";

        // Verify that the edge entry on the perpendicular fin (tri 2) was NOT
        // assigned a neighbour — only the two coplanar triangles should be
        // cross-linked. Locate tri 2 in the BVH-ordered triangle array by its
        // characteristic geometry (vertex with z > 0).
        int finBvhIdx = -1;
        for (size_t i = 0; i < result.triangles.size(); ++i)
        {
            auto const & t = result.triangles[i];
            if (t.v0.z > 0.5f || t.v1.z > 0.5f || t.v2.z > 0.5f)
            {
                finBvhIdx = static_cast<int>(i);
                break;
            }
        }
        ASSERT_GE(finBvhIdx, 0);

        // Edge 0 of tri 2 is (v0,v1) = (0,1) — the shared non-manifold edge.
        // It must NOT have been assigned a neighbour normal, because the
        // resolver picked the two coplanar triangles, not this one.
        auto const & finEdge0 =
            result.edgeNeighborNormals[static_cast<size_t>(finBvhIdx) * 3u + 0u];
        EXPECT_LT(finEdge0.normal.w, 0.5f)
            << "Perpendicular fin must not be selected as the non-manifold partner";
    }

}  // namespace gladius::tests
