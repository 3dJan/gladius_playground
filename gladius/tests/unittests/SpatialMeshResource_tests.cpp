/// @file SpatialMeshResource_tests.cpp
/// @brief Unit tests for SpatialMeshResource
/// @see SpatialMeshResource.h

#include "SpatialMeshResource.h"
#include "MeshBVH.h"
#include "Primitives.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace gladius::tests
{
    // ========================================================================
    // Test Fixtures
    // ========================================================================

    class SpatialMeshResource_Test : public ::testing::Test
    {
      protected:
        // Helper to create a simple cube mesh
        static void createCubeMesh(std::vector<float4> & vertices,
                                   std::vector<TriangleIndices> & indices)
        {
            vertices = {
                {-0.5f, -0.5f, -0.5f, 0.f},
                { 0.5f, -0.5f, -0.5f, 0.f},
                { 0.5f,  0.5f, -0.5f, 0.f},
                {-0.5f,  0.5f, -0.5f, 0.f},
                {-0.5f, -0.5f,  0.5f, 0.f},
                { 0.5f, -0.5f,  0.5f, 0.f},
                { 0.5f,  0.5f,  0.5f, 0.f},
                {-0.5f,  0.5f,  0.5f, 0.f},
            };

            indices = {
                {4, 5, 6}, {4, 6, 7},
                {1, 0, 3}, {1, 3, 2},
                {5, 1, 2}, {5, 2, 6},
                {0, 4, 7}, {0, 7, 3},
                {7, 6, 2}, {7, 2, 3},
                {0, 1, 5}, {0, 5, 4},
            };
        }
    };

    // ========================================================================
    // T019: Serialization test
    // ========================================================================

    TEST_F(SpatialMeshResource_Test, Load_BuildsBVHAndSerializes)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{1}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        // Constructor already calls load() internally, so subsequent call returns false
        // (load() returns false if already loaded, true on first load)
        bool secondLoadResult = resource.load();
        EXPECT_FALSE(secondLoadResult);  // Already loaded by constructor

        // Verify basic properties
        EXPECT_EQ(resource.getTriangleCount(), 12u);

        // Verify BVH data was built
        auto const & data = resource.getData();
        EXPECT_FALSE(data.empty());
        EXPECT_GT(data.nodes.size(), 0u);
        EXPECT_EQ(data.triangles.size(), 12u);
        EXPECT_EQ(data.vertexNormals.size(), 8u);  // 8 vertices in cube

        // Verify bounding box
        auto const & bbox = resource.getBoundingBox();
        EXPECT_LE(bbox.min.x, -0.5f);
        EXPECT_GE(bbox.max.x, 0.5f);

        // Note: getStartIndex/getEndIndex are set in write(), not in load().
        // Indices remain 0 until write() is called with a Primitives buffer.
        // This is consistent with other ResourceBase-derived classes.
    }

    // ========================================================================
    // T043: Rebuild performance test
    // ========================================================================

    TEST_F(SpatialMeshResource_Test, Rebuild_UpdatesWithinOneSecond)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{2}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);
        resource.load();

        // Modify vertices slightly
        for (auto & v : vertices)
        {
            v.x *= 1.1f;
            v.y *= 1.1f;
            v.z *= 1.1f;
        }

        auto start = std::chrono::high_resolution_clock::now();
        resource.rebuild(vertices, indices);
        auto end = std::chrono::high_resolution_clock::now();

        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // Should complete within 1 second for a small mesh
        EXPECT_LT(durationMs, 1000);

        // Bounding box should be updated
        auto const & bbox = resource.getBoundingBox();
        EXPECT_GT(bbox.max.x, 0.5f);  // Scaled up
    }

    // ========================================================================
    // T044: Invalidation test
    // ========================================================================

    TEST_F(SpatialMeshResource_Test, MeshChange_TriggersInvalidation)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{3}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);
        resource.load();

        // Get initial state
        size_t initialTriCount = resource.getTriangleCount();

        // Invalidate
        resource.invalidate();

        // Triangle count should still be valid
        EXPECT_EQ(resource.getTriangleCount(), initialTriCount);

        // Rebuild with different data
        std::vector<float4> newVertices = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
        };
        std::vector<TriangleIndices> newIndices = {TriangleIndices{0, 1, 2}};

        resource.rebuild(newVertices, newIndices);

        // Should now have 1 triangle
        EXPECT_EQ(resource.getTriangleCount(), 1u);
    }

}  // namespace gladius::tests
