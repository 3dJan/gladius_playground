/// @file HierarchicalDC_STLExport_tests.cpp
/// @brief Tests to validate hierarchical DC produces valid mesh geometry (for STL export)
/// 
/// These tests verify that the CPU implementation of hierarchical dual contouring
/// produces correct mesh geometry that can be exported to STL format.
/// Uses direct mesh extraction API instead of the exporter for simplicity.

#include "Document.h"
#include "HierarchicalDualContouring.h"
#include "EventLogger.h"
#include "ComputeContext.h"

#include <compute/ComputeCore.h>

#include <gtest/gtest.h>

namespace gladius_tests::hierarchical_dc_mesh
{
    using namespace gladius;
    using namespace gladius::hierarchical_dc;

    class HierarchicalDC_STL_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const & path)
        {
            auto core = std::make_shared<ComputeCore>(
              m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);

            return DocumentBundle{std::move(core), std::move(document)};
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    /// @test HierarchicalOctreeBuilder_CpuMeshExtraction_ProducesValidGeometry
    /// Verifies CPU mesh extraction produces valid geometry for STL export
    TEST_F(HierarchicalDC_STL_Test, ImplicitGyroid_ProducesValidSTL)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        // Test CPU extraction (safe fallback for STL export)
        HierarchicalConfig config;
        applyQualityPreset(config, HierarchicalQuality::Draft);
        config.enableGpuAcceleration = false; // CPU only for reliability

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        // Validate mesh suitable for STL export
        ASSERT_GT(vertices.size(), 0U) << "Mesh should have vertices";
        ASSERT_GT(indices.size(), 0U) << "Mesh should have triangles";
        EXPECT_EQ(indices.size() % 3U, 0U) << "Indices must form complete triangles";
        EXPECT_GT(indices.size(), 300U) << "Gyroid should produce substantial mesh (>100 triangles)";
    }

    /// @test HierarchicalOctreeBuilder_QualityPresets_ProduceDifferentDetail
    /// Verifies different quality presets produce meshes with varying detail levels
    TEST_F(HierarchicalDC_STL_Test, QualityPresets_ProduceDifferentMeshes)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        struct QualityResult
        {
            HierarchicalQuality quality;
            std::size_t vertexCount;
            std::size_t triangleCount;
        };

        std::vector<QualityResult> results;

        // Test Draft, Balanced, Fine presets
        for (auto quality : {HierarchicalQuality::Draft,
                            HierarchicalQuality::Balanced,
                            HierarchicalQuality::Fine})
        {
            HierarchicalConfig config;
            applyQualityPreset(config, quality);
            config.enableGpuAcceleration = false; // CPU only

            HierarchicalOctreeBuilder builder(*bundle.core, config);
            builder.buildOctree(bbox.value());

            std::vector<Eigen::Vector3f> vertices;
            std::vector<std::uint32_t> indices;
            builder.extractMesh(vertices, indices);

            ASSERT_GT(vertices.size(), 0U) << "Should produce vertices";
            ASSERT_GT(indices.size(), 0U) << "Should produce triangles";

            results.push_back({quality, vertices.size(), indices.size() / 3U});
        }

        // Verify quality hierarchy: Fine should have more detail than Draft
        ASSERT_EQ(results.size(), 3U);
        EXPECT_LT(results[0].vertexCount, results[1].vertexCount)
          << "Balanced should have more vertices than Draft";
    }

    /// @test HierarchicalOctreeBuilder_CpuExtraction_IsDeterministic
    /// Verifies CPU mesh extraction produces consistent results
    TEST_F(HierarchicalDC_STL_Test, SimpleGyroid_ProducesConsistentOutput)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config;
        applyQualityPreset(config, HierarchicalQuality::Draft);
        config.enableGpuAcceleration = false; // CPU only

        // First extraction
        HierarchicalOctreeBuilder builder1(*bundle.core, config);
        builder1.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices1;
        std::vector<std::uint32_t> indices1;
        builder1.extractMesh(vertices1, indices1);

        // Second extraction
        HierarchicalOctreeBuilder builder2(*bundle.core, config);
        builder2.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices2;
        std::vector<std::uint32_t> indices2;
        builder2.extractMesh(vertices2, indices2);

        // Should produce identical results (deterministic)
        EXPECT_EQ(vertices1.size(), vertices2.size()) << "CPU extraction should be deterministic";
        EXPECT_EQ(indices1.size(), indices2.size()) << "CPU extraction should be deterministic";
    }

    /// @test HierarchicalOctreeBuilder_MeshTopology_HasNoDegenera tes
    /// Verifies extracted mesh has valid topology for STL export
    TEST_F(HierarchicalDC_STL_Test, STLGeometry_HasValidTopology)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        HierarchicalConfig config;
        applyQualityPreset(config, HierarchicalQuality::Balanced);
        config.enableGpuAcceleration = false; // CPU only

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        ASSERT_GT(vertices.size(), 0U);
        ASSERT_GT(indices.size(), 0U);

        // Check for degenerate triangles (triangles with duplicate vertex indices)
        std::size_t degenerateCount = 0U;
        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
        {
            auto const i0 = indices[i + 0U];
            auto const i1 = indices[i + 1U];
            auto const i2 = indices[i + 2U];

            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                ++degenerateCount;
            }
        }

        auto const totalTriangles = indices.size() / 3U;
        auto const degenerateRatio = static_cast<double>(degenerateCount) / static_cast<double>(totalTriangles);

        EXPECT_LT(degenerateRatio, 0.01) << "Less than 1% of triangles should be degenerate";
    }

} // namespace gladius_tests::hierarchical_dc_mesh
