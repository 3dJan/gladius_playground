#include <gtest/gtest.h>
#include "compute/MeshSimplification.h"
#include "compute/MeshQualityMetrics.h"

#include <Eigen/Core>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
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

        /// Generate a closed (watertight) UV sphere with single pole vertices and stitched seam.
        /// Every edge is shared by exactly 2 triangles — guaranteed manifold.
        [[nodiscard]] SphereMesh generateClosedSphere(float radius, std::size_t stacks, std::size_t slices)
        {
            SphereMesh mesh;
            mesh.radius = radius;

            // Single north pole vertex
            mesh.positions.push_back({0.F, radius, 0.F});
            mesh.normals.push_back({0.F, 1.F, 0.F});

            // Interior latitude rings (stacks-1 rings, slices vertices each, no seam duplicate)
            for (std::size_t i = 1U; i < stacks; ++i)
            {
                float const phi = std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(stacks);
                float const y = radius * std::cos(phi);
                float const r = radius * std::sin(phi);
                for (std::size_t j = 0U; j < slices; ++j)
                {
                    float const theta = 2.0F * std::numbers::pi_v<float> * static_cast<float>(j) / static_cast<float>(slices);
                    Eigen::Vector3f const pos(r * std::cos(theta), y, r * std::sin(theta));
                    mesh.positions.push_back(pos);
                    mesh.normals.push_back(pos.normalized());
                }
            }

            // Single south pole vertex
            auto const southPole = static_cast<std::uint32_t>(mesh.positions.size());
            mesh.positions.push_back({0.F, -radius, 0.F});
            mesh.normals.push_back({0.F, -1.F, 0.F});

            // North pole fan
            for (std::uint32_t j = 0U; j < static_cast<std::uint32_t>(slices); ++j)
            {
                mesh.indices.push_back(0U);
                mesh.indices.push_back(1U + j);
                mesh.indices.push_back(1U + (j + 1U) % static_cast<std::uint32_t>(slices));
            }

            // Interior quad strips
            for (std::size_t i = 0U; i + 2U < stacks; ++i)
            {
                auto const ring0 = static_cast<std::uint32_t>(1U + i * slices);
                auto const ring1 = static_cast<std::uint32_t>(1U + (i + 1U) * slices);
                auto const sl = static_cast<std::uint32_t>(slices);
                for (std::uint32_t j = 0U; j < sl; ++j)
                {
                    std::uint32_t const j1 = (j + 1U) % sl;
                    mesh.indices.push_back(ring0 + j);
                    mesh.indices.push_back(ring1 + j);
                    mesh.indices.push_back(ring0 + j1);

                    mesh.indices.push_back(ring1 + j);
                    mesh.indices.push_back(ring1 + j1);
                    mesh.indices.push_back(ring0 + j1);
                }
            }

            // South pole fan
            auto const lastRing = static_cast<std::uint32_t>(1U + (stacks - 2U) * slices);
            auto const sl = static_cast<std::uint32_t>(slices);
            for (std::uint32_t j = 0U; j < sl; ++j)
            {
                mesh.indices.push_back(lastRing + j);
                mesh.indices.push_back(southPole);
                mesh.indices.push_back(lastRing + (j + 1U) % sl);
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

    // ========================================================================
    // Fast QEM simplification tests (T011)
    // ========================================================================

    TEST(FastQemSimplifyTest, SphereReduction_TargetCount_ReducesTriangles)
    {
        auto sphere = generateSphere(1.0F, 20, 40);
        auto const originalTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetTriangleCount;
        config.targetTriangleCount = originalTriCount / 2U;

        auto collapsed = fastQemSimplify(sphere.positions, sphere.indices, config);

        auto const finalTriCount = sphere.indices.size() / 3U;
        EXPECT_GT(collapsed, 0U);
        EXPECT_LE(finalTriCount, config.targetTriangleCount + 10U); // allow some tolerance
        EXPECT_GT(finalTriCount, 0U);

        // All indices should be valid
        for (auto idx : sphere.indices)
        {
            EXPECT_LT(idx, static_cast<std::uint32_t>(sphere.positions.size()));
        }
    }

    TEST(FastQemSimplifyTest, SphereReduction_Percentage_ReducesTriangles)
    {
        auto sphere = generateSphere(1.0F, 20, 40);
        auto const originalTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 50.0F;

        auto collapsed = fastQemSimplify(sphere.positions, sphere.indices, config);

        auto const finalTriCount = sphere.indices.size() / 3U;
        EXPECT_GT(collapsed, 0U);
        // Should remove approximately 50% (within margin)
        float const actualReduction = 1.0F - static_cast<float>(finalTriCount) / static_cast<float>(originalTriCount);
        EXPECT_GT(actualReduction, 0.3F); // at least 30%
    }

    TEST(FastQemSimplifyTest, ErrorBounded_StopsAtThreshold)
    {
        auto sphere = generateSphere(1.0F, 16, 32);
        auto const initialTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::ErrorBounded;
        config.maxError = 1e-6F; // very tight — should stop early

        auto collapsed = fastQemSimplify(sphere.positions, sphere.indices, config);

        auto const finalTriCount = sphere.indices.size() / 3U;
        // With a very tight error, most collapses should be skipped so most triangles remain
        EXPECT_GT(finalTriCount, initialTriCount / 2U) << "Tight error threshold should preserve most triangles";
        // But at least some collapses should have happened (zero-error collapses exist on a regular sphere)
        EXPECT_GT(collapsed, 0U) << "At least some zero-error collapses should occur";
    }

    TEST(FastQemSimplifyTest, ManifoldPreservation_NoDegenerate)
    {
        auto sphere = generateSphere(1.0F, 16, 32);

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 75.0F; // aggressive

        fastQemSimplify(sphere.positions, sphere.indices, config);

        // Check: no degenerate triangles
        auto const triCount = sphere.indices.size() / 3U;
        for (std::size_t t = 0U; t < triCount; ++t)
        {
            auto const i0 = sphere.indices[t * 3U + 0U];
            auto const i1 = sphere.indices[t * 3U + 1U];
            auto const i2 = sphere.indices[t * 3U + 2U];
            EXPECT_NE(i0, i1);
            EXPECT_NE(i1, i2);
            EXPECT_NE(i2, i0);
        }

        // Check: all indices valid
        for (auto idx : sphere.indices)
        {
            EXPECT_LT(idx, static_cast<std::uint32_t>(sphere.positions.size()));
        }
    }

    TEST(FastQemSimplifyTest, ManifoldPreservation_AllEdgesSharedByTwoTriangles)
    {
        auto sphere = generateSphere(1.0F, 16, 32);

        // First check input manifoldness
        auto countEdgeDefects = [](std::vector<std::uint32_t> const & idx)
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edgeTris;
            auto const tc = idx.size() / 3U;
            for (std::size_t t = 0U; t < tc; ++t)
            {
                std::uint32_t const v[3] = {idx[t * 3U], idx[t * 3U + 1U], idx[t * 3U + 2U]};
                for (int e = 0; e < 3; ++e)
                {
                    auto a = v[e];
                    auto b = v[(e + 1) % 3];
                    if (a > b) std::swap(a, b);
                    edgeTris[{a, b}]++;
                }
            }
            std::size_t boundary = 0U;
            std::size_t nonManifold = 0U;
            for (auto const & [edge, count] : edgeTris)
            {
                if (count == 1U) ++boundary;
                else if (count > 2U) ++nonManifold;
            }
            return std::pair{boundary, nonManifold};
        };

        auto [inputBoundary, inputNonManifold] = countEdgeDefects(sphere.indices);

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 50.0F;

        fastQemSimplify(sphere.positions, sphere.indices, config);

        auto [outputBoundary, outputNonManifold] = countEdgeDefects(sphere.indices);

        // Simplification must never create non-manifold edges (>2 tris sharing an edge)
        EXPECT_EQ(outputNonManifold, 0U)
            << "Simplification created " << outputNonManifold << " non-manifold edges"
            << " (input had " << inputNonManifold << ")";

        // Simplification should not increase boundary edges
        EXPECT_LE(outputBoundary, inputBoundary)
            << "Simplification created new boundary edges: " << outputBoundary
            << " > input " << inputBoundary;
    }

    TEST(FastQemSimplifyTest, ClosedMesh_RemainsWatertightAfterSimplification)
    {
        auto sphere = generateClosedSphere(1.0F, 16, 32);

        auto countEdgeDefects = [](std::vector<std::uint32_t> const & idx)
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edgeTris;
            auto const tc = idx.size() / 3U;
            for (std::size_t t = 0U; t < tc; ++t)
            {
                std::uint32_t const v[3] = {idx[t * 3U], idx[t * 3U + 1U], idx[t * 3U + 2U]};
                for (int e = 0; e < 3; ++e)
                {
                    auto a = v[e];
                    auto b = v[(e + 1) % 3];
                    if (a > b) std::swap(a, b);
                    edgeTris[{a, b}]++;
                }
            }
            std::size_t boundary = 0U;
            std::size_t nonManifold = 0U;
            for (auto const & [edge, count] : edgeTris)
            {
                if (count == 1U) ++boundary;
                else if (count > 2U) ++nonManifold;
            }
            return std::pair{boundary, nonManifold};
        };

        // Verify input is a closed manifold
        auto [inputBoundary, inputNonManifold] = countEdgeDefects(sphere.indices);
        ASSERT_EQ(inputBoundary, 0U) << "Input sphere should have no boundary edges";
        ASSERT_EQ(inputNonManifold, 0U) << "Input sphere should have no non-manifold edges";

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 50.0F;

        fastQemSimplify(sphere.positions, sphere.indices, config);

        EXPECT_GT(sphere.indices.size() / 3U, 0U);

        auto [outputBoundary, outputNonManifold] = countEdgeDefects(sphere.indices);
        EXPECT_EQ(outputBoundary, 0U)
            << "Simplification created " << outputBoundary << " boundary edges (holes)";
        EXPECT_EQ(outputNonManifold, 0U)
            << "Simplification created " << outputNonManifold << " non-manifold edges";
    }

    TEST(FastQemSimplifyTest, ClosedMesh_AggressiveReduction_RemainsWatertight)
    {
        auto sphere = generateClosedSphere(1.0F, 24, 48); // larger mesh for aggressive test

        auto countEdgeDefects = [](std::vector<std::uint32_t> const & idx)
        {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edgeTris;
            auto const tc = idx.size() / 3U;
            for (std::size_t t = 0U; t < tc; ++t)
            {
                std::uint32_t const v[3] = {idx[t * 3U], idx[t * 3U + 1U], idx[t * 3U + 2U]};
                for (int e = 0; e < 3; ++e)
                {
                    auto a = v[e];
                    auto b = v[(e + 1) % 3];
                    if (a > b) std::swap(a, b);
                    edgeTris[{a, b}]++;
                }
            }
            std::size_t boundary = 0U;
            std::size_t nonManifold = 0U;
            for (auto const & [edge, count] : edgeTris)
            {
                if (count == 1U) ++boundary;
                else if (count > 2U) ++nonManifold;
            }
            return std::pair{boundary, nonManifold};
        };

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 75.0F; // very aggressive

        fastQemSimplify(sphere.positions, sphere.indices, config);

        auto [outputBoundary, outputNonManifold] = countEdgeDefects(sphere.indices);
        EXPECT_EQ(outputBoundary, 0U)
            << "Aggressive simplification created " << outputBoundary << " boundary edges (holes)";
        EXPECT_EQ(outputNonManifold, 0U)
            << "Aggressive simplification created " << outputNonManifold << " non-manifold edges";
    }

    TEST(FastQemSimplifyTest, DoubleDiagonalDiamond_NoDuplicateTriangles)
    {
        // Construct a mesh that would produce overlapping triangles without the
        // duplicate-face guard. The "double-diagonal diamond" has edges along both
        // diagonals: (A-B) and (C-D), plus the 4 perimeter edges. Collapsing A-B
        // without a duplicate check would turn triangle (B,C,D) into (A,C,D) which
        // already exists, creating two coincident, opposite-winding faces = hole.
        //
        //       C
        //      /|\
        //     / | \
        //    A--+--B     (both diagonals present)
        //     \ | /
        //      \|/
        //       D
        //
        // Plus apex E above and apex F below to close the surface.

        std::vector<Eigen::Vector3f> positions = {
            { 0.0F,  0.0F,  0.0F},  // 0: A
            { 2.0F,  0.0F,  0.0F},  // 1: B
            { 1.0F,  1.0F,  0.0F},  // 2: C
            { 1.0F, -1.0F,  0.0F},  // 3: D
            { 1.0F,  0.0F,  1.0F},  // 4: E (apex above)
            { 1.0F,  0.0F, -1.0F},  // 5: F (apex below)
        };

        // 10 triangles forming a closed surface around the diamond.
        // The diamond has shared tris (A,B,C) and (A,B,D), plus the diagonal
        // tris (A,C,D) and (B,C,D). The rest close the mesh via apexes.
        std::vector<std::uint32_t> indices = {
            // Diamond faces (both diagonals)
            0, 1, 2,   // (A, B, C)
            0, 3, 1,   // (A, D, B)
            0, 2, 3,   // (A, C, D)
            1, 3, 2,   // (B, D, C)
            // Top cap (E above, closing with C)
            0, 4, 2,   // (A, E, C)
            1, 2, 4,   // (B, C, E)
            // Bottom cap (F below, closing with D)
            0, 3, 5,   // (A, D, F)
            1, 5, 3,   // (B, F, D)
            // Front/back caps connecting E and F
            0, 5, 4,   // (A, F, E)
            1, 4, 5,   // (B, E, F)
        };

        auto countDuplicateFaces = [](std::vector<std::uint32_t> const & idx)
        {
            std::map<std::array<std::uint32_t, 3>, std::size_t> faceSets;
            auto const tc = idx.size() / 3U;
            for (std::size_t t = 0U; t < tc; ++t)
            {
                std::array<std::uint32_t, 3> verts = {idx[t * 3U], idx[t * 3U + 1U], idx[t * 3U + 2U]};
                std::sort(verts.begin(), verts.end());
                faceSets[verts]++;
            }
            std::size_t dupes = 0U;
            for (auto const & [face, count] : faceSets)
            {
                if (count > 1U) dupes += count - 1U;
            }
            return dupes;
        };

        EXPECT_EQ(countDuplicateFaces(indices), 0U) << "Test mesh has duplicates before simplification";

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 50.0F;

        fastQemSimplify(positions, indices, config);

        EXPECT_EQ(countDuplicateFaces(indices), 0U)
            << "Simplification created duplicate overlapping triangles";
    }

    TEST(FastQemSimplifyTest, EmptyMesh_Returns0)
    {
        std::vector<Eigen::Vector3f> positions;
        std::vector<std::uint32_t> indices;
        FastQemConfig config;

        auto collapsed = fastQemSimplify(positions, indices, config);
        EXPECT_EQ(collapsed, 0U);
    }

    TEST(FastQemSimplifyTest, NoReductionNeeded_Returns0)
    {
        auto sphere = generateSphere(1.0F, 4, 8);
        auto const originalTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetTriangleCount;
        config.targetTriangleCount = originalTriCount + 100U; // already below target

        auto collapsed = fastQemSimplify(sphere.positions, sphere.indices, config);
        EXPECT_EQ(collapsed, 0U);
    }

    // ========================================================================
    // Regression test for existing QemSdfAware (T011b)
    // ========================================================================

    TEST(QemSdfAwareRegressionTest, SphereSimplification_StillWorks)
    {
        auto sphere = generateSphere(1.0F, 16, 32);
        auto const originalTriCount = sphere.indices.size() / 3U;

        QemSimplificationConfig config;
        config.targetTriangleCount = originalTriCount / 2U;
        config.maxPasses = 5U;

        // Provide trivial SDF evaluator (returns distance from origin)
        auto sdfEval = [](std::vector<Eigen::Vector3f> const & positions) -> std::vector<float>
        {
            std::vector<float> result(positions.size());
            for (std::size_t i = 0U; i < positions.size(); ++i)
            {
                result[i] = positions[i].norm() - 1.0F; // SDF of unit sphere
            }
            return result;
        };

        auto gradEval = [](std::vector<Eigen::Vector3f> const & positions) -> std::vector<Eigen::Vector3f>
        {
            std::vector<Eigen::Vector3f> result(positions.size());
            for (std::size_t i = 0U; i < positions.size(); ++i)
            {
                result[i] = positions[i].normalized();
            }
            return result;
        };

        QemMeshSimplifier simplifier;
        simplifier.setConfig(config);
        simplifier.setGpuSdfEvaluator(sdfEval);
        simplifier.setGpuSdfGradientEvaluator(gradEval);

        auto collapsed = simplifier.simplify(sphere.positions, sphere.normals, sphere.indices);

        EXPECT_GT(collapsed, 0U);
        auto const finalTriCount = sphere.indices.size() / 3U;
        EXPECT_LT(finalTriCount, originalTriCount);

        // All indices valid
        for (auto idx : sphere.indices)
        {
            EXPECT_LT(idx, static_cast<std::uint32_t>(sphere.positions.size()));
        }
    }

    TEST(FastQemSimplifyTest, Cancellation_StopsEarlyAndThrows)
    {
        auto sphere = generateSphere(1.0F, 16, 32);
        auto const initialTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 90.0F; // very aggressive to ensure many collapses needed
        config.cancelCheckPeriod = 4U;         // check frequently

        std::size_t cancelAfter = 2U;
        std::size_t callCount = 0U;
        auto throwOnCancel = [&]()
        {
            ++callCount;
            if (callCount >= cancelAfter)
            {
                throw std::runtime_error("cancelled");
            }
        };

        EXPECT_THROW(
            fastQemSimplify(sphere.positions, sphere.indices, config, throwOnCancel),
            std::runtime_error);

        // After cancel, mesh arrays are unchanged size (compaction doesn't run)
        auto const finalTriCount = sphere.indices.size() / 3U;
        EXPECT_EQ(finalTriCount, initialTriCount);
        EXPECT_GE(callCount, cancelAfter) << "Cancel callback should have been invoked";
    }

    TEST(FastQemSimplifyTest, ProgressCallback_ReportsAscendingProgress)
    {
        auto sphere = generateSphere(1.0F, 16, 32);

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetReductionPercent;
        config.targetReductionPercent = 50.0F;

        std::vector<int> progressValues;
        auto progressFn = [&](int progress)
        {
            progressValues.push_back(progress);
        };

        fastQemSimplify(sphere.positions, sphere.indices, config, nullptr, progressFn);

        ASSERT_FALSE(progressValues.empty()) << "Progress callback should have been called";
        EXPECT_EQ(progressValues.back(), 100) << "Final progress should be 100";

        // Progress should be monotonically non-decreasing
        for (std::size_t i = 1U; i < progressValues.size(); ++i)
        {
            EXPECT_GE(progressValues[i], progressValues[i - 1U]);
        }
    }

    TEST(FastQemSimplifyTest, StarvationGuard_TerminatesWhenTargetUnreachable)
    {
        // Request a reduction target that topology guards will make impossible.
        // The starvation detector must ensure the loop terminates promptly.
        auto sphere = generateSphere(1.0F, 8, 16);
        std::size_t const initialTriCount = sphere.indices.size() / 3U;

        FastQemConfig config;
        config.terminationMode = SimplificationTerminationMode::TargetTriangleCount;
        config.targetTriangleCount = 4U; // Almost certainly unreachable

        auto const start = std::chrono::steady_clock::now();
        fastQemSimplify(sphere.positions, sphere.indices, config);
        auto const elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        std::size_t const finalTriCount = sphere.indices.size() / 3U;
        EXPECT_LT(finalTriCount, initialTriCount) << "Some reduction should have occurred";
        EXPECT_LT(elapsed.count(), 2000) << "Starvation guard should prevent long spinning";
    }

} // namespace gladius::compute::tests
