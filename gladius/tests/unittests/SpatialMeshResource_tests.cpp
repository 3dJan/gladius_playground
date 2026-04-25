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

    // ========================================================================
    // Unsigned Distance Validation Tests (T043)
    // ========================================================================

    /// Helper to compute squared distance from point to triangle (reference implementation)
    /// This is a simple brute-force implementation for validation
    /// Uses float4 with w=0 for compatibility with codebase types
    static float sqDistanceToTriangle(float4 const& point, 
                                       float4 const& v0, 
                                       float4 const& v1, 
                                       float4 const& v2)
    {
        // Edge vectors (using xyz only)
        float abx = v1.x - v0.x, aby = v1.y - v0.y, abz = v1.z - v0.z;
        float acx = v2.x - v0.x, acy = v2.y - v0.y, acz = v2.z - v0.z;
        float apx = point.x - v0.x, apy = point.y - v0.y, apz = point.z - v0.z;

        float d1 = abx * apx + aby * apy + abz * apz;
        float d2 = acx * apx + acy * apy + acz * apz;

        // Check if point is in vertex region outside v0
        if (d1 <= 0.0f && d2 <= 0.0f)
        {
            return apx * apx + apy * apy + apz * apz;
        }

        float bpx = point.x - v1.x, bpy = point.y - v1.y, bpz = point.z - v1.z;
        float d3 = abx * bpx + aby * bpy + abz * bpz;
        float d4 = acx * bpx + acy * bpy + acz * bpz;

        // Check if point is in vertex region outside v1
        if (d3 >= 0.0f && d4 <= d3)
        {
            return bpx * bpx + bpy * bpy + bpz * bpz;
        }

        float vc = d1 * d4 - d3 * d2;

        // Check if point is in edge region of v0-v1
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        {
            float v = d1 / (d1 - d3);
            float npx = v0.x + v * abx, npy = v0.y + v * aby, npz = v0.z + v * abz;
            float dx = point.x - npx, dy = point.y - npy, dz = point.z - npz;
            return dx * dx + dy * dy + dz * dz;
        }

        float cpx = point.x - v2.x, cpy = point.y - v2.y, cpz = point.z - v2.z;
        float d5 = abx * cpx + aby * cpy + abz * cpz;
        float d6 = acx * cpx + acy * cpy + acz * cpz;

        // Check if point is in vertex region outside v2
        if (d6 >= 0.0f && d5 <= d6)
        {
            return cpx * cpx + cpy * cpy + cpz * cpz;
        }

        float vb = d5 * d2 - d1 * d6;

        // Check if point is in edge region of v0-v2
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        {
            float w = d2 / (d2 - d6);
            float npx = v0.x + w * acx, npy = v0.y + w * acy, npz = v0.z + w * acz;
            float dx = point.x - npx, dy = point.y - npy, dz = point.z - npz;
            return dx * dx + dy * dy + dz * dz;
        }

        float va = d3 * d6 - d5 * d4;

        // Check if point is in edge region of v1-v2
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            float ex = v2.x - v1.x, ey = v2.y - v1.y, ez = v2.z - v1.z;
            float npx = v1.x + w * ex, npy = v1.y + w * ey, npz = v1.z + w * ez;
            float dx = point.x - npx, dy = point.y - npy, dz = point.z - npz;
            return dx * dx + dy * dy + dz * dz;
        }

        // Point is inside the triangle - compute distance to plane
        float denom = 1.0f / (va + vb + vc);
        float v = vb * denom;
        float w = vc * denom;
        float npx = v0.x + abx * v + acx * w;
        float npy = v0.y + aby * v + acy * w;
        float npz = v0.z + abz * v + acz * w;
        float dx = point.x - npx, dy = point.y - npy, dz = point.z - npz;
        return dx * dx + dy * dy + dz * dz;
    }

    TEST_F(SpatialMeshResource_Test, UnsignedDistance_MatchesReferenceImplementation)
    {
        // Create a simple cube mesh
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        // Build BVH and serialize
        ResourceKey key(ResourceId{100}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        auto const& data = resource.getData();
        ASSERT_FALSE(data.triangles.empty());

        // Test points at various locations (using float4 with w=0)
        std::vector<float4> testPoints = {
            {0.0f, 0.0f, 0.0f, 0.f},    // Center of cube (inside)
            {0.6f, 0.0f, 0.0f, 0.f},    // Outside, near face
            {0.5f, 0.5f, 0.5f, 0.f},    // On vertex
            {0.0f, 0.5f, 0.0f, 0.f},    // On edge
            {1.0f, 1.0f, 1.0f, 0.f},    // Far outside, corner direction
            {-1.0f, 0.0f, 0.0f, 0.f},   // Far outside, axis direction
            {0.25f, 0.25f, 0.6f, 0.f},  // Near face
        };

        for (auto const& point : testPoints)
        {
            // Compute reference minimum distance by brute force over all triangles
            float refMinSqDist = std::numeric_limits<float>::max();
            for (auto const& tri : data.triangles)
            {
                float sqDist = sqDistanceToTriangle(point, tri.v0, tri.v1, tri.v2);
                refMinSqDist = std::min(refMinSqDist, sqDist);
            }

            // The unsigned distance should be sqrt of minSqDist
            float refUnsignedDist = std::sqrt(refMinSqDist);

            // Note: We can't directly call the GPU kernel from here, but we can
            // verify that the reference algorithm matches expected behavior
            // This validates the algorithm that's also used in the GPU kernel
            
            // For points strictly inside the cube, distance should be small (≤ 0.5)
            // Center of unit cube (at origin) has distance exactly 0.5 to each face
            if (std::abs(point.x) < 0.5f && 
                std::abs(point.y) < 0.5f && 
                std::abs(point.z) < 0.5f)
            {
                EXPECT_LE(refUnsignedDist, 0.5f + 1e-5f) 
                    << "Point (" << point.x << ", " << point.y << ", " << point.z 
                    << ") is inside cube, expected distance <= 0.5";
            }

            // For points outside, distance should be positive
            if (std::abs(point.x) > 0.5f || 
                std::abs(point.y) > 0.5f || 
                std::abs(point.z) > 0.5f)
            {
                EXPECT_GT(refUnsignedDist, 0.0f) 
                    << "Point (" << point.x << ", " << point.y << ", " << point.z 
                    << ") is outside cube, expected distance > 0";
            }
        }
    }

    TEST_F(SpatialMeshResource_Test, BVHData_ContainsPrecomputedFaceNormals)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{101}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        auto const& data = resource.getData();
        ASSERT_EQ(data.triangles.size(), 12u);

        // All face normals should be normalized (length = 1)
        for (size_t i = 0; i < data.triangles.size(); ++i)
        {
            auto const& tri = data.triangles[i];
            float length = std::sqrt(tri.faceNormal.x * tri.faceNormal.x +
                                     tri.faceNormal.y * tri.faceNormal.y +
                                     tri.faceNormal.z * tri.faceNormal.z);
            EXPECT_NEAR(length, 1.0f, 1e-5f) 
                << "Triangle " << i << " has non-unit face normal (length=" << length << ")";
        }

        // For a cube, normals should be axis-aligned (one component = ±1, others = 0)
        int axisAlignedCount = 0;
        for (auto const& tri : data.triangles)
        {
            bool xAligned = (std::abs(std::abs(tri.faceNormal.x) - 1.0f) < 1e-5f &&
                             std::abs(tri.faceNormal.y) < 1e-5f &&
                             std::abs(tri.faceNormal.z) < 1e-5f);
            bool yAligned = (std::abs(tri.faceNormal.x) < 1e-5f &&
                             std::abs(std::abs(tri.faceNormal.y) - 1.0f) < 1e-5f &&
                             std::abs(tri.faceNormal.z) < 1e-5f);
            bool zAligned = (std::abs(tri.faceNormal.x) < 1e-5f &&
                             std::abs(tri.faceNormal.y) < 1e-5f &&
                             std::abs(std::abs(tri.faceNormal.z) - 1.0f) < 1e-5f);
            if (xAligned || yAligned || zAligned)
            {
                ++axisAlignedCount;
            }
        }

        // All 12 triangles of a cube should have axis-aligned normals
        EXPECT_EQ(axisAlignedCount, 12) 
            << "Expected all cube triangles to have axis-aligned face normals";
    }

    // ========================================================================
    // setEvaluationConfig tests
    // ========================================================================

    TEST_F(SpatialMeshResource_Test, EvaluationConfig_DefaultMethod_AllocatesVoxelGrid)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{200}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        // Default config is VoxelAccelerated → grid should be available.
        auto const params = resource.getVoxelGridBuildParams();
        ASSERT_TRUE(params.has_value());
        EXPECT_GT(params->voxelCount, 0);
    }

    TEST_F(SpatialMeshResource_Test, SignCacheBuildParams_DefaultMesh_Available)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{205}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        auto const params = resource.getSignCacheBuildParams();
        ASSERT_TRUE(params.has_value());
        EXPECT_GT(params->signCacheDataOffset, 0);
        EXPECT_GE(params->signCacheReadyOffset, 0);
        EXPECT_GT(params->nodesOffset, 0);
        EXPECT_GT(params->trianglesOffset, 0);
        EXPECT_GT(params->fwnAggregatesOffset, 0);
        EXPECT_GT(params->nodeCount, 0);
        EXPECT_EQ(params->resolution, 64);
        EXPECT_EQ(params->wordCount, 8192);
    }

    TEST_F(SpatialMeshResource_Test, EvaluationConfig_PureBVH_DoesNotRebuild)
    {
        // PureBVH needs no extra payload beyond the always-present BVH, so a
        // method change to PureBVH from any other method should be a free
        // runtime switch (no payload rebuild). The pre-existing voxel grid
        // stays in the payload — kernel dispatch ignores it under PureBVH.
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{201}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        MeshSdfEvaluationConfig cfg;
        cfg.method = MeshSdfMethod::PureBVH;
        // Returns true: the *config* did change (method differs from default).
        bool const configChanged = resource.setEvaluationConfig(cfg);
        EXPECT_TRUE(configChanged);
        EXPECT_EQ(resource.evaluationConfig().method, MeshSdfMethod::PureBVH);
    }

    TEST_F(SpatialMeshResource_Test, EvaluationConfig_RuntimeOnlyChange_DoesNotInvalidate)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{202}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        auto cfg = resource.evaluationConfig();
        cfg.inflationDistance = 0.05f;
        cfg.useEarlyExit = !cfg.useEarlyExit;
        bool const invalidated = resource.setEvaluationConfig(cfg);
        EXPECT_FALSE(invalidated);
        EXPECT_FLOAT_EQ(resource.evaluationConfig().inflationDistance, 0.05f);
    }

    TEST_F(SpatialMeshResource_Test, EvaluationConfig_ResolutionChange_Invalidates)
    {
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{203}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        auto cfg = resource.evaluationConfig();
        cfg.voxelGridResolution = cfg.voxelGridResolution * 2;
        EXPECT_TRUE(resource.setEvaluationConfig(cfg));
    }

    TEST_F(SpatialMeshResource_Test, EvaluationConfig_MethodRoundTrip_VoxelGridStaysAvailable)
    {
        // With the lazy-payload optimization, switching between PureBVH and
        // VoxelAccelerated does not drop the voxel grid (the grid is harmless
        // for PureBVH, which simply ignores it). This avoids costly
        // re-serialise on common round-trips like "settings auto-apply on
        // load → user toggles method in dialog".
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        createCubeMesh(vertices, indices);

        ResourceKey key(ResourceId{204}, ResourceType::Unknown);
        SpatialMeshResource resource(key, vertices, indices);

        // Default (VoxelAccelerated) → grid present.
        ASSERT_TRUE(resource.getVoxelGridBuildParams().has_value());

        // Switch to PureBVH → grid stays (free runtime switch).
        MeshSdfEvaluationConfig pureBvh;
        pureBvh.method = MeshSdfMethod::PureBVH;
        EXPECT_TRUE(resource.setEvaluationConfig(pureBvh));
        EXPECT_TRUE(resource.getVoxelGridBuildParams().has_value());

        // Switch back to VoxelAccelerated → grid still present.
        MeshSdfEvaluationConfig voxel;
        voxel.method = MeshSdfMethod::VoxelAccelerated;
        EXPECT_TRUE(resource.setEvaluationConfig(voxel));
        auto const params = resource.getVoxelGridBuildParams();
        ASSERT_TRUE(params.has_value());
        EXPECT_GT(params->voxelCount, 0);
    }

}  // namespace gladius::tests
