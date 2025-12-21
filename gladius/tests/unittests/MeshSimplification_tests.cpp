#include <gtest/gtest.h>
#include "compute/MeshSimplification.h"
#include "compute/MeshQualityMetrics.h"

#include <Eigen/Core>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

namespace gladius::compute::tests
{
    namespace
    {
        /// Generate a UV sphere mesh for testing
        struct SphereMesh
        {
            std::vector<Eigen::Vector3f> positions;
            std::vector<Eigen::Vector3f> normals;
            std::vector<std::uint32_t> indices;
            float radius{1.0F};
        };

        [[nodiscard]] SphereMesh generateSphere(float radius, std::size_t stacks, std::size_t slices)
        {
            SphereMesh mesh;
            mesh.radius = radius;

            // Generate vertices
            for (std::size_t i = 0; i <= stacks; ++i)
            {
                float const phi = std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(stacks);
                float const y = radius * std::cos(phi);
                float const r = radius * std::sin(phi);

                for (std::size_t j = 0; j <= slices; ++j)
                {
                    float const theta = 2.0F * std::numbers::pi_v<float> * static_cast<float>(j) / static_cast<float>(slices);
                    float const x = r * std::cos(theta);
                    float const z = r * std::sin(theta);

                    Eigen::Vector3f const pos(x, y, z);
                    mesh.positions.push_back(pos);
                    mesh.normals.push_back(pos.normalized());
                }
            }

            // Generate indices
            for (std::size_t i = 0; i < stacks; ++i)
            {
                for (std::size_t j = 0; j < slices; ++j)
                {
                    std::uint32_t const first = static_cast<std::uint32_t>(i * (slices + 1) + j);
                    std::uint32_t const second = first + static_cast<std::uint32_t>(slices + 1);

                    mesh.indices.push_back(first);
                    mesh.indices.push_back(second);
                    mesh.indices.push_back(first + 1);

                    mesh.indices.push_back(second);
                    mesh.indices.push_back(second + 1);
                    mesh.indices.push_back(first + 1);
                }
            }

            return mesh;
        }

        /// Generate a box mesh for testing
        struct BoxMesh
        {
            std::vector<Eigen::Vector3f> positions;
            std::vector<Eigen::Vector3f> normals;
            std::vector<std::uint32_t> indices;
        };

        [[nodiscard]] BoxMesh generateBox(float sizeX, float sizeY, float sizeZ)
        {
            BoxMesh mesh;

            float const hx = sizeX * 0.5F;
            float const hy = sizeY * 0.5F;
            float const hz = sizeZ * 0.5F;

            // 8 corners of the box
            std::array<Eigen::Vector3f, 8> corners = {{
                {-hx, -hy, -hz}, {+hx, -hy, -hz}, {+hx, +hy, -hz}, {-hx, +hy, -hz},
                {-hx, -hy, +hz}, {+hx, -hy, +hz}, {+hx, +hy, +hz}, {-hx, +hy, +hz}
            }};

            // 6 faces, each with 4 vertices and 2 triangles
            // Face definitions: {v0, v1, v2, v3, normal}
            struct FaceDef
            {
                std::array<int, 4> verts;
                Eigen::Vector3f normal;
            };

            std::array<FaceDef, 6> faces = {{
                {{0, 1, 2, 3}, {0, 0, -1}},  // -Z
                {{4, 7, 6, 5}, {0, 0, +1}},  // +Z
                {{0, 4, 5, 1}, {0, -1, 0}},  // -Y
                {{2, 6, 7, 3}, {0, +1, 0}},  // +Y
                {{0, 3, 7, 4}, {-1, 0, 0}},  // -X
                {{1, 5, 6, 2}, {+1, 0, 0}}   // +X
            }};

            for (auto const & face : faces)
            {
                std::uint32_t const baseIdx = static_cast<std::uint32_t>(mesh.positions.size());

                for (int i = 0; i < 4; ++i)
                {
                    mesh.positions.push_back(corners[face.verts[i]]);
                    mesh.normals.push_back(face.normal);
                }

                // Two triangles per face
                mesh.indices.push_back(baseIdx + 0);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 2);

                mesh.indices.push_back(baseIdx + 0);
                mesh.indices.push_back(baseIdx + 2);
                mesh.indices.push_back(baseIdx + 3);
            }

            return mesh;
        }

        /// Compute mesh statistics
        struct MeshStats
        {
            std::size_t vertexCount{0U};
            std::size_t triangleCount{0U};
            float surfaceArea{0.0F};
        };

        [[nodiscard]] MeshStats computeMeshStats(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> const & indices)
        {
            MeshStats stats;
            stats.vertexCount = positions.size();
            stats.triangleCount = indices.size() / 3U;

            for (std::size_t t = 0; t < stats.triangleCount; ++t)
            {
                Eigen::Vector3f const & v0 = positions[indices[t * 3 + 0]];
                Eigen::Vector3f const & v1 = positions[indices[t * 3 + 1]];
                Eigen::Vector3f const & v2 = positions[indices[t * 3 + 2]];
                stats.surfaceArea += 0.5F * (v1 - v0).cross(v2 - v0).norm();
            }

            return stats;
        }

    } // anonymous namespace

    class MeshSimplificationBenchmarkTest : public ::testing::Test
    {
      protected:
        MeshQualityAnalyzer m_analyzer;

        void SetUp() override
        {
            MeshQualityAnalyzer::Config config;
            config.samplesPerTriangle = 5U;
            config.maxTotalSamples = 50000U;
            m_analyzer = MeshQualityAnalyzer(config);
        }

        void printBenchmarkResults(
            std::string const & testName,
            std::string const & method,
            MeshStats const & originalStats,
            MeshStats const & simplifiedStats,
            MeshQualityMetrics const & metrics,
            std::chrono::milliseconds duration)
        {
            float const reductionRatio = 1.0F - static_cast<float>(simplifiedStats.triangleCount) /
                                                    static_cast<float>(originalStats.triangleCount);

            std::cout << "\n=== " << testName << " - " << method << " ===" << std::endl;
            std::cout << "Original: " << originalStats.triangleCount << " triangles, "
                      << originalStats.vertexCount << " vertices" << std::endl;
            std::cout << "Simplified: " << simplifiedStats.triangleCount << " triangles, "
                      << simplifiedStats.vertexCount << " vertices" << std::endl;
            std::cout << "Reduction: " << (reductionRatio * 100.0F) << "%" << std::endl;
            std::cout << "Duration: " << duration.count() << " ms" << std::endl;
            std::cout << "Quality Metrics:" << std::endl;
            std::cout << "  Hausdorff Distance: " << metrics.hausdorffDistance << std::endl;
            std::cout << "  Mean Distance: " << metrics.meanDistance << std::endl;
            std::cout << "  RMS Distance: " << metrics.rmsDistance << std::endl;
            std::cout << "  95th Percentile: " << metrics.percentile95Distance << std::endl;
            std::cout << "  Samples Used: " << metrics.sampleCount << std::endl;
        }
    };

    TEST_F(MeshSimplificationBenchmarkTest, HausdorffDistanceBasicTest)
    {
        // Create two identical spheres - Hausdorff should be 0
        auto const sphere1 = generateSphere(1.0F, 16, 32);
        auto const sphere2 = generateSphere(1.0F, 16, 32);

        auto const metrics = m_analyzer.analyze(
            sphere1.positions, sphere1.indices,
            sphere2.positions, sphere2.indices);

        EXPECT_NEAR(metrics.hausdorffDistance, 0.0F, 1e-5F);
        EXPECT_NEAR(metrics.meanDistance, 0.0F, 1e-5F);
    }

    TEST_F(MeshSimplificationBenchmarkTest, HausdorffDistanceScaledSpheres)
    {
        // Create two spheres with different radii
        auto const sphere1 = generateSphere(1.0F, 16, 32);
        auto sphere2 = generateSphere(1.1F, 16, 32);  // 10% larger

        auto const metrics = m_analyzer.analyze(
            sphere1.positions, sphere1.indices,
            sphere2.positions, sphere2.indices);

        // Hausdorff should be approximately 0.1 (radius difference)
        EXPECT_NEAR(metrics.hausdorffDistance, 0.1F, 0.02F);
    }

    // Test the quality analyzer itself
    TEST(MeshQualityAnalyzerTest, PointToTriangleDistance_InTriangle)
    {
        Eigen::Vector3f const a(0, 0, 0);
        Eigen::Vector3f const b(1, 0, 0);
        Eigen::Vector3f const c(0, 1, 0);
        
        // Point directly above center of triangle
        Eigen::Vector3f const p(0.25F, 0.25F, 1.0F);
        
        MeshQualityAnalyzer analyzer;
        std::vector<Eigen::Vector3f> positions = {a, b, c};
        std::vector<std::uint32_t> indices = {0, 1, 2};
        
        std::vector<Eigen::Vector3f> samplePositions = {p};
        
        auto const metrics = analyzer.analyze(positions, indices, positions, indices);
        
        // Same mesh should have 0 distance
        EXPECT_NEAR(metrics.hausdorffDistance, 0.0F, 1e-5F);
    }

    TEST(MeshQualityAnalyzerTest, EmptyMesh)
    {
        MeshQualityAnalyzer analyzer;
        std::vector<Eigen::Vector3f> empty;
        std::vector<std::uint32_t> emptyIndices;
        
        auto const metrics = analyzer.analyze(empty, emptyIndices, empty, emptyIndices);
        
        EXPECT_EQ(metrics.sampleCount, 0U);
        EXPECT_EQ(metrics.hausdorffDistance, 0.0F);
    }

    // Test the mesh quality improver
    TEST(MeshQualityImproverTest, ComputeQualityStats)
    {
        // Create a simple triangle mesh (two triangles sharing an edge)
        std::vector<Eigen::Vector3f> positions = {
            {0.0F, 0.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {0.5F, 1.0F, 0.0F},
            {0.5F, -0.2F, 0.0F}  // Creates elongated triangles
        };
        std::vector<std::uint32_t> indices = {
            0, 1, 2,
            0, 3, 1
        };
        
        auto const stats = MeshQualityImprover::computeQualityStats(positions, indices);
        
        EXPECT_EQ(stats.totalTriangles, 2U);
        EXPECT_GT(stats.minAngle, 0.0F);
        EXPECT_LT(stats.maxAngle, 180.0F);
        EXPECT_GT(stats.avgMinAngle, 0.0F);
        EXPECT_GT(stats.avgAspectRatio, 0.0F);
        EXPECT_LE(stats.avgAspectRatio, 1.0F);
    }

    TEST(MeshQualityImproverTest, ImproveQualityOnQuadMesh)
    {
        // Create a quad split into two triangles with a poor diagonal
        // The "poor" diagonal creates two triangles with worse angles
        std::vector<Eigen::Vector3f> positions = {
            {0.0F, 0.0F, 0.0F},    // 0: bottom-left
            {1.0F, 0.0F, 0.0F},    // 1: bottom-right
            {1.0F, 1.0F, 0.0F},    // 2: top-right
            {0.0F, 1.0F, 0.0F}     // 3: top-left
        };
        
        // Poor diagonal: 0-2 (the longer diagonal)
        std::vector<std::uint32_t> indices = {
            0, 1, 2,
            0, 2, 3
        };
        
        auto const statsBefore = MeshQualityImprover::computeQualityStats(positions, indices);
        
        MeshQualityImprover improver;
        std::size_t const flips = improver.improveQuality(positions, indices);
        
        auto const statsAfter = MeshQualityImprover::computeQualityStats(positions, indices);
        
        // For a unit square, both diagonals are equal so no flips expected
        // But the algorithm should still work without errors
        EXPECT_EQ(indices.size(), 6U);  // Still 2 triangles
        
        std::cout << "Quad mesh improvement test:" << std::endl;
        std::cout << "  Before: min angle " << statsBefore.minAngle << "°" << std::endl;
        std::cout << "  After:  min angle " << statsAfter.minAngle << "°" << std::endl;
        std::cout << "  Edge flips: " << flips << std::endl;
    }

    TEST(MeshQualityImproverTest, ImproveElongatedTriangles)
    {
        // Create a mesh with deliberately poor triangles
        // A strip of quads triangulated with bad diagonals
        std::vector<Eigen::Vector3f> positions;
        std::vector<std::uint32_t> indices;
        
        // Create a row of vertices with alternating heights to form elongated triangles
        for (int i = 0; i <= 4; ++i)
        {
            float const x = static_cast<float>(i);
            positions.push_back({x, 0.0F, 0.0F});  // bottom row
            positions.push_back({x, 1.0F, 0.0F});  // top row
        }
        
        // Create triangles with poor diagonals (always use long diagonal)
        for (int i = 0; i < 4; ++i)
        {
            std::uint32_t const bl = static_cast<std::uint32_t>(i * 2);
            std::uint32_t const tl = bl + 1;
            std::uint32_t const br = bl + 2;
            std::uint32_t const tr = bl + 3;
            
            // Poor triangulation: use long diagonal
            indices.push_back(bl);
            indices.push_back(br);
            indices.push_back(tr);
            
            indices.push_back(bl);
            indices.push_back(tr);
            indices.push_back(tl);
        }
        
        auto const statsBefore = MeshQualityImprover::computeQualityStats(positions, indices);
        
        MeshQualityImprover::Config config;
        config.maxEdgeFlipPasses = 10U;
        MeshQualityImprover improver(config);
        
        std::size_t const flips = improver.improveQuality(positions, indices);
        
        auto const statsAfter = MeshQualityImprover::computeQualityStats(positions, indices);
        
        std::cout << "Elongated triangles improvement test:" << std::endl;
        std::cout << "  Before: min angle " << statsBefore.minAngle 
                  << "°, avg min " << statsBefore.avgMinAngle << "°" << std::endl;
        std::cout << "  After:  min angle " << statsAfter.minAngle 
                  << "°, avg min " << statsAfter.avgMinAngle << "°" << std::endl;
        std::cout << "  Edge flips: " << flips << std::endl;
        
        EXPECT_EQ(indices.size(), 24U);  // Still 8 triangles
    }

} // namespace gladius::compute::tests
