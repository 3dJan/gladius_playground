/// @file MeshRepair_tests.cpp
/// @brief Unit tests for the mesh repair module.
/// @see MeshRepair.h

#include "MeshRepair.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gladius::tests
{
    using mesh_repair::MeshRepairConfig;
    using mesh_repair::MeshRepairResult;

    namespace
    {
        std::vector<float4> cubeVertices()
        {
            return {
                {-0.5f, -0.5f, -0.5f, 0.f},
                { 0.5f, -0.5f, -0.5f, 0.f},
                { 0.5f,  0.5f, -0.5f, 0.f},
                {-0.5f,  0.5f, -0.5f, 0.f},
                {-0.5f, -0.5f,  0.5f, 0.f},
                { 0.5f, -0.5f,  0.5f, 0.f},
                { 0.5f,  0.5f,  0.5f, 0.f},
                {-0.5f,  0.5f,  0.5f, 0.f},
            };
        }
        std::vector<TriangleIndices> cubeIndices()
        {
            return {
                {4, 5, 6}, {4, 6, 7}, // +z
                {1, 0, 3}, {1, 3, 2}, // -z
                {5, 1, 2}, {5, 2, 6}, // +x
                {0, 4, 7}, {0, 7, 3}, // -x
                {7, 6, 2}, {7, 2, 3}, // +y
                {0, 1, 5}, {0, 5, 4}, // -y
            };
        }
    } // namespace

    // ========================================================================
    // weldVertices
    // ========================================================================

    TEST(MeshRepair_WeldVertices, DuplicatesWithinEpsilon_Merged)
    {
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
            {1.0000001f, 0.0000001f, 0.f, 0.f}, // duplicate of vertex 1
        };
        std::vector<TriangleIndices> indices = {
            {0, 1, 2},
            {0, 3, 2},
        };

        std::size_t const removed = mesh_repair::weldVertices(vertices, indices, 1e-3f);

        EXPECT_EQ(removed, 1u);
        EXPECT_EQ(vertices.size(), 3u);
        // Both triangles must now reference the same merged vertex (was 1 / 3).
        EXPECT_EQ(indices[0].i1, indices[1].i1);
    }

    TEST(MeshRepair_WeldVertices, DistinctVertices_Preserved)
    {
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        std::size_t const removed = mesh_repair::weldVertices(vertices, indices, 1e-5f);
        EXPECT_EQ(removed, 0u);
        EXPECT_EQ(vertices.size(), 8u);
    }

    // ========================================================================
    // removeDegenerateTriangles
    // ========================================================================

    TEST(MeshRepair_RemoveDegenerateTriangles, ZeroArea_Removed)
    {
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
            {2.f, 0.f, 0.f, 0.f},
        };
        std::vector<TriangleIndices> indices = {
            {0, 1, 2},   // valid
            {0, 1, 3},   // collinear → zero area
            {0, 0, 1},   // collapsed indices
        };

        std::size_t const removed =
            mesh_repair::removeDegenerateTriangles(vertices, indices, 1e-10f);

        EXPECT_EQ(removed, 2u);
        ASSERT_EQ(indices.size(), 1u);
        EXPECT_EQ(indices[0].i0, 0);
        EXPECT_EQ(indices[0].i1, 1);
        EXPECT_EQ(indices[0].i2, 2);
    }

    TEST(MeshRepair_RemoveDegenerateTriangles, SmallButValid_Preserved)
    {
        std::vector<float4> vertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
        };
        std::vector<TriangleIndices> indices = {{0, 1, 2}};
        std::size_t const removed =
            mesh_repair::removeDegenerateTriangles(vertices, indices, 1e-10f);
        EXPECT_EQ(removed, 0u);
        EXPECT_EQ(indices.size(), 1u);
    }

    // ========================================================================
    // orientConsistently
    // ========================================================================

    TEST(MeshRepair_OrientConsistently, CubeWithOneFlippedTriangle_Reoriented)
    {
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        // Flip triangle 0
        std::swap(indices[0].i1, indices[0].i2);

        std::size_t const flipped = mesh_repair::orientConsistently(vertices, indices);

        // One flip needed to restore consistency.
        EXPECT_EQ(flipped, 1u);
        // Triangle 0 should match the original orientation again.
        EXPECT_EQ(indices[0].i0, 4);
        EXPECT_EQ(indices[0].i1, 5);
        EXPECT_EQ(indices[0].i2, 6);
    }

    TEST(MeshRepair_OrientConsistently, AlreadyConsistent_NoFlips)
    {
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        std::size_t const flipped = mesh_repair::orientConsistently(vertices, indices);
        EXPECT_EQ(flipped, 0u);
    }

    // ========================================================================
    // fillSmallHoles
    // ========================================================================

    TEST(MeshRepair_FillSmallHoles, OpenQuad_Filled)
    {
        // Cube with one face missing (the +z face). Six boundary edges form one loop.
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        // Remove the +z face (first 2 triangles).
        indices.erase(indices.begin(), indices.begin() + 2);

        // Hole loop is the +z square: perimeter ~ 4.0. Use a generous threshold.
        auto const r = mesh_repair::fillSmallHoles(vertices, indices, 10.f);

        EXPECT_EQ(r.filled, 1u);
        EXPECT_GE(r.added, 4u); // at least 4 fan triangles around the centroid
    }

    TEST(MeshRepair_FillSmallHoles, LargeHole_Skipped)
    {
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        indices.erase(indices.begin(), indices.begin() + 2);

        // Very tight perimeter limit: the +z face has perimeter 4, well above 0.5.
        auto const r = mesh_repair::fillSmallHoles(vertices, indices, 0.5f);

        EXPECT_EQ(r.filled, 0u);
        EXPECT_EQ(r.added, 0u);
    }

    // ========================================================================
    // repairMesh orchestrator
    // ========================================================================

    TEST(MeshRepair_RepairMesh, AllStepsDisabled_NoChange)
    {
        std::vector<float4> vertices = cubeVertices();
        std::vector<TriangleIndices> indices = cubeIndices();
        std::size_t const vBefore = vertices.size();
        std::size_t const iBefore = indices.size();

        MeshRepairResult const r = mesh_repair::repairMesh(vertices, indices, MeshRepairConfig{});

        EXPECT_EQ(r.weldedVertices, 0u);
        EXPECT_EQ(r.removedTriangles, 0u);
        EXPECT_EQ(r.flippedTriangles, 0u);
        EXPECT_EQ(r.filledHoles, 0u);
        EXPECT_EQ(vertices.size(), vBefore);
        EXPECT_EQ(indices.size(), iBefore);
    }

    TEST(MeshRepair_RepairMesh, BrokenCube_AllStepsCombined_RestoresClosed)
    {
        // Construct a cube with: duplicated vertex, degenerate triangle, flipped
        // triangle, and an open face.
        std::vector<float4> vertices = cubeVertices();
        vertices.push_back(vertices[0]); // duplicate of vertex 0
        std::vector<TriangleIndices> indices = cubeIndices();
        indices.push_back({0, 0, 1});       // degenerate
        std::swap(indices[2].i1, indices[2].i2); // flip a -z triangle
        indices.erase(indices.begin(), indices.begin() + 2); // remove +z face

        MeshRepairConfig cfg;
        cfg.weld = true;
        cfg.weldEpsilon = 1e-5f;
        cfg.removeDegenerate = true;
        cfg.areaEpsilon = 1e-10f;
        cfg.orientConsistently = true;
        cfg.fillHoles = true;
        cfg.maxHolePerimeter = 10.f;

        MeshRepairResult const r = mesh_repair::repairMesh(vertices, indices, cfg);

        EXPECT_GE(r.weldedVertices, 1u);
        EXPECT_GE(r.removedTriangles, 1u);
        EXPECT_GE(r.flippedTriangles, 1u);
        EXPECT_GE(r.filledHoles, 1u);
        // The mesh must now be closed: every directed edge has a matching opposite.
        std::vector<std::pair<int, int>> dirEdges;
        dirEdges.reserve(indices.size() * 3u);
        for (auto const & t : indices)
        {
            dirEdges.emplace_back(t.i0, t.i1);
            dirEdges.emplace_back(t.i1, t.i2);
            dirEdges.emplace_back(t.i2, t.i0);
        }
        std::size_t openEdges = 0u;
        for (auto const & e : dirEdges)
        {
            std::pair<int, int> const opposite{e.second, e.first};
            if (std::find(dirEdges.begin(), dirEdges.end(), opposite) == dirEdges.end())
            {
                ++openEdges;
            }
        }
        EXPECT_EQ(openEdges, 0u) << "Repaired mesh must be closed";
    }

} // namespace gladius::tests
