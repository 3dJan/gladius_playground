/// @file HierarchicalDualContouring_tests.cpp
/// @brief Unit tests for hierarchical dual contouring implementation

#include "Document.h"
#include "HierarchicalDualContouring.h"
#include "EventLogger.h"
#include "ComputeContext.h"

#include <compute/ComputeCore.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

namespace gladius_tests::hierarchical_dc
{
    using namespace gladius::hierarchical_dc;

    namespace
    {
        constexpr std::uint32_t kTestMaxDepth = 6U;

        void clampMaxDepthForTests(HierarchicalConfig & config)
        {
            if (config.maxDepth > kTestMaxDepth)
            {
                config.maxDepth = kTestMaxDepth;
            }

            if (config.initialDepth > config.maxDepth)
            {
                config.initialDepth = config.maxDepth;
            }
        }
    } // namespace

    class HierarchicalDualContouring_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<gladius::events::Logger>();
            m_context = std::make_shared<gladius::ComputeContext>(gladius::EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        struct DocumentBundle
        {
            std::shared_ptr<gladius::ComputeCore> core;
            std::shared_ptr<gladius::Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const & path)
        {
            auto core = std::make_shared<gladius::ComputeCore>(
              m_context, gladius::RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<gladius::Document>(core);
            document->load(path);

            return DocumentBundle{std::move(core), std::move(document)};
        }

        std::shared_ptr<gladius::ComputeContext> m_context;
        gladius::events::SharedLogger m_logger;
    };

    /// @test HierarchicalOctreeBuilder_WithDraftPreset_ProducesValidOctree
    TEST_F(HierarchicalDualContouring_Test, WithDraftPreset_ProducesValidOctree)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Draft);
        config.enableGpuAcceleration = true;
        // Test-only: ensure depth stays reasonable
        if (config.maxDepth > 5U) config.maxDepth = 5U;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        auto const & stats = builder.getStats();
        
        EXPECT_GT(stats.totalNodes, 0U);
        EXPECT_GT(stats.leafNodes, 0U);
        EXPECT_LE(stats.deepestLevel, config.initialDepth);
        EXPECT_GT(stats.totalCornerQueries, 0U);
    }

    /// @test HierarchicalOctreeBuilder_WithBalancedPreset_PerformsAdaptiveRefinement
    TEST_F(HierarchicalDualContouring_Test, WithBalancedPreset_PerformsAdaptiveRefinement)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        auto const & stats = builder.getStats();
        
        EXPECT_GT(stats.totalNodes, 0U);
        EXPECT_GT(stats.leafNodes, 0U);
        EXPECT_GT(stats.intersectingLeaves, 0U);
        EXPECT_EQ(stats.refinementPasses, config.refinementIterations);
        EXPECT_GT(stats.totalGradientQueries, 0U);
        EXPECT_GT(stats.totalConstructionTimeMs, 0.0);
    }

    /// @test HierarchicalOctreeBuilder_ExtractMesh_ProducesNonEmptyMesh
    TEST_F(HierarchicalDualContouring_Test, ExtractMesh_ProducesNonEmptyMesh)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        EXPECT_GT(vertices.size(), 0U);
        EXPECT_GT(indices.size(), 0U);
        EXPECT_EQ(indices.size() % 3U, 0U) << "Indices must form complete triangles";

        // Verify all indices are in range
        for (auto const idx : indices)
        {
            EXPECT_LT(idx, vertices.size()) << "Index out of range";
        }
    }

    /// @test HierarchicalOctreeBuilder_WithFinePreset_ProducesDeeperTree
    TEST_F(HierarchicalDualContouring_Test, WithFinePreset_ProducesDeeperTree)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Fine);
        config.enableGpuAcceleration = true;
        // Test-only: cap depth to control memory
        if (config.initialDepth > 4U) config.initialDepth = 4U;
        if (config.maxDepth > 6U) config.maxDepth = 6U;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        auto const & stats = builder.getStats();
        
        EXPECT_GT(stats.totalNodes, 0U);
        EXPECT_GT(stats.leafNodes, 0U);
        EXPECT_GE(stats.deepestLevel, 5U) << "Fine preset should reach at least depth 5 (with coarsening enabled)";
        EXPECT_LE(stats.deepestLevel, config.maxDepth);
    }

    /// @test HierarchicalOctreeBuilder_CpuFallback_WorksWithoutGpu
    TEST_F(HierarchicalDualContouring_Test, CpuFallback_WorksWithoutGpu)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Draft);
        config.enableGpuAcceleration = false; // Force CPU fallback
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        auto const & stats = builder.getStats();
        
        EXPECT_GT(stats.totalNodes, 0U);
        EXPECT_GT(stats.leafNodes, 0U);
        EXPECT_GT(stats.totalCornerQueries, 0U);
    }

    /// @test HierarchicalOctreeBuilder_GpuVsCpu_ProducesSimilarResults
    TEST_F(HierarchicalDualContouring_Test, GpuVsCpu_ProducesSimilarResults)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        // GPU build
        HierarchicalConfig gpuConfig{};
        applyQualityPreset(gpuConfig, HierarchicalQuality::Draft);
        gpuConfig.enableGpuAcceleration = true;
        if (gpuConfig.maxDepth > 5U) gpuConfig.maxDepth = 5U;
        clampMaxDepthForTests(gpuConfig);

        HierarchicalOctreeBuilder gpuBuilder(*bundle.core, gpuConfig);
        gpuBuilder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> gpuVertices;
        std::vector<std::uint32_t> gpuIndices;
        gpuBuilder.extractMesh(gpuVertices, gpuIndices);

        // CPU build
        HierarchicalConfig cpuConfig{};
        applyQualityPreset(cpuConfig, HierarchicalQuality::Draft);
        cpuConfig.enableGpuAcceleration = false;
        if (cpuConfig.maxDepth > 5U) cpuConfig.maxDepth = 5U;
        clampMaxDepthForTests(cpuConfig);

        HierarchicalOctreeBuilder cpuBuilder(*bundle.core, cpuConfig);
        cpuBuilder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> cpuVertices;
        std::vector<std::uint32_t> cpuIndices;
        cpuBuilder.extractMesh(cpuVertices, cpuIndices);

        // Results should be similar (within 10% tolerance)
        EXPECT_NEAR(static_cast<double>(gpuVertices.size()),
                    static_cast<double>(cpuVertices.size()),
                    static_cast<double>(gpuVertices.size()) * 0.1);
        
        EXPECT_NEAR(static_cast<double>(gpuIndices.size()),
                    static_cast<double>(cpuIndices.size()),
                    static_cast<double>(gpuIndices.size()) * 0.1);
    }

    /// @test HierarchicalOctreeBuilder_MeshTopology_ProducesValidTriangles
    TEST_F(HierarchicalDualContouring_Test, MeshTopology_ProducesValidTriangles)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        ASSERT_GT(indices.size(), 0U);
        ASSERT_EQ(indices.size() % 3U, 0U);

        // Check for degenerate triangles
        std::size_t degenerateCount = 0U;
        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
        {
            auto const & v0 = vertices[indices[i + 0U]];
            auto const & v1 = vertices[indices[i + 1U]];
            auto const & v2 = vertices[indices[i + 2U]];

            Eigen::Vector3f const edge1 = v1 - v0;
            Eigen::Vector3f const edge2 = v2 - v0;
            
            // Compute cross product manually to avoid Eigen version issues
            Eigen::Vector3f normal;
            normal.x() = edge1.y() * edge2.z() - edge1.z() * edge2.y();
            normal.y() = edge1.z() * edge2.x() - edge1.x() * edge2.z();
            normal.z() = edge1.x() * edge2.y() - edge1.y() * edge2.x();

            if (normal.squaredNorm() <= 1e-12F)
            {
                ++degenerateCount;
            }
        }

        std::size_t const triangleCount = indices.size() / 3U;
        float const degenerateRatio = static_cast<float>(degenerateCount) / static_cast<float>(triangleCount);
        
        EXPECT_LT(degenerateRatio, 0.01F) << "Too many degenerate triangles: " << degenerateCount;
    }

    /// @test HierarchicalOctreeBuilder_VertexPlacement_RemainsWithinBounds
    TEST_F(HierarchicalDualContouring_Test, VertexPlacement_RemainsWithinBounds)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        Eigen::Vector3f const bboxMin{bbox.value().min.s[0], bbox.value().min.s[1], bbox.value().min.s[2]};
        Eigen::Vector3f const bboxMax{bbox.value().max.s[0], bbox.value().max.s[1], bbox.value().max.s[2]};

        // Allow 1% tolerance for vertices slightly outside bounds
        Eigen::Vector3f const extent = bboxMax - bboxMin;
        Eigen::Vector3f const tolerance = extent * 0.01F;

        for (auto const & vertex : vertices)
        {
            EXPECT_GE(vertex.x(), bboxMin.x() - tolerance.x());
            EXPECT_LE(vertex.x(), bboxMax.x() + tolerance.x());
            EXPECT_GE(vertex.y(), bboxMin.y() - tolerance.y());
            EXPECT_LE(vertex.y(), bboxMax.y() + tolerance.y());
            EXPECT_GE(vertex.z(), bboxMin.z() - tolerance.z());
            EXPECT_LE(vertex.z(), bboxMax.z() + tolerance.z());
        }
    }

    /// @test HierarchicalOctreeBuilder_AdaptiveRefinement_IncreasesNodeCount
    TEST_F(HierarchicalDualContouring_Test, AdaptiveRefinement_IncreasesNodeCount)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        // Build without refinement
        HierarchicalConfig noRefineConfig{};
        applyQualityPreset(noRefineConfig, HierarchicalQuality::Draft);
        noRefineConfig.enableGpuAcceleration = true;
        noRefineConfig.refinementIterations = 0U;
        if (noRefineConfig.maxDepth > 5U) noRefineConfig.maxDepth = 5U;
        clampMaxDepthForTests(noRefineConfig);

        HierarchicalOctreeBuilder noRefineBuilder(*bundle.core, noRefineConfig);
        noRefineBuilder.buildOctree(bbox.value());
        auto const & noRefineStats = noRefineBuilder.getStats();

        // Build with refinement
        HierarchicalConfig refineConfig{};
        applyQualityPreset(refineConfig, HierarchicalQuality::Balanced);
        refineConfig.enableGpuAcceleration = true;
        refineConfig.refinementIterations = 2U;
        if (refineConfig.initialDepth > 4U) refineConfig.initialDepth = 4U;
        if (refineConfig.maxDepth > 5U) refineConfig.maxDepth = 5U;
        clampMaxDepthForTests(refineConfig);

        HierarchicalOctreeBuilder refineBuilder(*bundle.core, refineConfig);
        refineBuilder.buildOctree(bbox.value());
        auto const & refineStats = refineBuilder.getStats();

        EXPECT_GT(refineStats.totalNodes, noRefineStats.totalNodes)
          << "Adaptive refinement should increase node count";
        EXPECT_GT(refineStats.totalGradientQueries, 0U)
          << "Adaptive refinement should perform gradient queries";
    }

    /// @test HierarchicalOctreeBuilder_SimpleGyroid_ProducesConsistentMesh
    TEST_F(HierarchicalDualContouring_Test, SimpleGyroid_ProducesConsistentMesh)
    {
        auto bundle = loadDocument("testdata/SimpleGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        // Build twice to ensure consistency
        HierarchicalOctreeBuilder builder1(*bundle.core, config);
        builder1.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices1;
        std::vector<std::uint32_t> indices1;
        builder1.extractMesh(vertices1, indices1);

        HierarchicalOctreeBuilder builder2(*bundle.core, config);
        builder2.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices2;
        std::vector<std::uint32_t> indices2;
        builder2.extractMesh(vertices2, indices2);

        EXPECT_EQ(vertices1.size(), vertices2.size())
          << "Consistent builds should produce same vertex count";
        EXPECT_EQ(indices1.size(), indices2.size())
          << "Consistent builds should produce same index count";
    }

    /// @test HierarchicalOctreeBuilder_PerformanceStats_ArePopulated
    TEST_F(HierarchicalDualContouring_Test, PerformanceStats_ArePopulated)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config{};
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = true;
        clampMaxDepthForTests(config);

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        auto const & stats = builder.getStats();

        EXPECT_GT(stats.totalNodes, 0U);
        EXPECT_GT(stats.leafNodes, 0U);
        EXPECT_LE(stats.leafNodes, stats.totalNodes);
        EXPECT_LE(stats.intersectingLeaves, stats.leafNodes);
        EXPECT_GT(stats.deepestLevel, 0U);
        EXPECT_GT(stats.totalCornerQueries, 0U);
        EXPECT_GE(stats.totalGradientQueries, 0U);
        EXPECT_EQ(stats.refinementPasses, config.refinementIterations);
        EXPECT_GT(stats.totalConstructionTimeMs, 0.0);
    }

} // namespace gladius_tests::hierarchical_dc
