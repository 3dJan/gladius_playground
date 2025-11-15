#include "HierarchicalDualContouring.h"

#include "Document.h"
#include "EventLogger.h"
#include "ComputeContext.h"
#include "compute/ComputeCore.h"

#include <gtest/gtest.h>

#include <optional>

using namespace gladius;
using namespace gladius::hierarchical_dc;

namespace gladius_tests::hierarchical_dc_mesh
{
    class HierarchicalDC_ExtractionStepsTest : public ::testing::Test
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

        DocumentBundle loadImplicitGyroid()
        {
            auto core = std::make_shared<ComputeCore>(
              m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load("testdata/ImplicitGyroid.3mf");
            return DocumentBundle{std::move(core), std::move(document)};
        }

        [[nodiscard]] HierarchicalConfig makeBalancedConfig() const
        {
            HierarchicalConfig config;
            applyQualityPreset(config, HierarchicalQuality::Balanced);
            config.enableGpuAcceleration = true;
            config.projectVerticesToSurface = false;
            return config;
        }

        [[nodiscard]] std::optional<BoundingBox> tryComputeBoundingBox(DocumentBundle & bundle)
        {
            if (!bundle.core->updateBBox())
            {
                return std::nullopt;
            }

            return bundle.core->getBoundingBox();
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    TEST_F(HierarchicalDC_ExtractionStepsTest, VertexIndexMap_CreatesDenseVertexBuffer)
    {
        auto bundle = loadImplicitGyroid();
        auto const bbox = tryComputeBoundingBox(bundle);
        if (!bbox.has_value())
        {
            GTEST_SKIP() << "Unable to determine bounding box";
        }

        auto config = makeBalancedConfig();
        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        ASSERT_FALSE(vertices.empty());

        auto const leafIndices = builder.getLeafIndices();
        auto const & nodes = builder.getNodes();

        std::size_t expectedVertexCount = 0U;
        for (auto idx : leafIndices)
        {
            OctreeNode const & node = nodes[idx];
            if (node.hasVertex && node.vertexPosition.has_value())
            {
                ++expectedVertexCount;
            }
        }

        EXPECT_EQ(vertices.size(), expectedVertexCount)
          << "Vertex buffer should be dense and match leaf vertices";
    }

    TEST_F(HierarchicalDC_ExtractionStepsTest, TopologyEmission_ProducesValidTriangles)
    {
        auto bundle = loadImplicitGyroid();
        auto const bbox = tryComputeBoundingBox(bundle);
        if (!bbox.has_value())
        {
            GTEST_SKIP() << "Unable to determine bounding box";
        }

        auto config = makeBalancedConfig();
        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        ASSERT_FALSE(indices.empty());
        ASSERT_EQ(indices.size() % 3U, 0U) << "Indices must form triangles";

        for (std::uint32_t index : indices)
        {
            EXPECT_LT(index, vertices.size()) << "Triangle references invalid vertex";
        }

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

                EXPECT_EQ(degenerateCount, 0U)
                    << "Dual contouring should emit zero degenerate triangles";
    }

    TEST_F(HierarchicalDC_ExtractionStepsTest, ProjectionToggle_DoesNotChangeTopology)
    {
        auto bundleA = loadImplicitGyroid();
        auto const bboxA = tryComputeBoundingBox(bundleA);
        if (!bboxA.has_value())
        {
            GTEST_SKIP() << "Unable to determine bounding box";
        }
        auto configNoProjection = makeBalancedConfig();
        configNoProjection.projectVerticesToSurface = false;

        HierarchicalOctreeBuilder builderNoProjection(*bundleA.core, configNoProjection);
        builderNoProjection.buildOctree(bboxA.value());

        std::vector<Eigen::Vector3f> verticesA;
        std::vector<std::uint32_t> indicesA;
        builderNoProjection.extractMesh(verticesA, indicesA);

        auto bundleB = loadImplicitGyroid();
        auto const bboxB = tryComputeBoundingBox(bundleB);
        if (!bboxB.has_value())
        {
            GTEST_SKIP() << "Unable to determine bounding box";
        }
        auto configProjection = makeBalancedConfig();
        configProjection.projectVerticesToSurface = true;

        HierarchicalOctreeBuilder builderProjection(*bundleB.core, configProjection);
        builderProjection.buildOctree(bboxB.value());

        std::vector<Eigen::Vector3f> verticesB;
        std::vector<std::uint32_t> indicesB;
        builderProjection.extractMesh(verticesB, indicesB);

                auto countDegenerates = [](std::vector<std::uint32_t> const & indices) {
                        std::size_t count = 0U;
                        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
                        {
                                auto const i0 = indices[i + 0U];
                                auto const i1 = indices[i + 1U];
                                auto const i2 = indices[i + 2U];
                                if (i0 == i1 || i1 == i2 || i2 == i0)
                                {
                                        ++count;
                                }
                        }
                        return count;
                };

                EXPECT_EQ(countDegenerates(indicesA), 0U)
                    << "Non-projected extraction should not emit degenerate triangles";
                EXPECT_EQ(countDegenerates(indicesB), 0U)
                    << "Projected extraction should not emit degenerate triangles";

                auto const triangleCountA = indicesA.size() / 3U;
                auto const triangleCountB = indicesB.size() / 3U;
                auto const maxCount = std::max(triangleCountA, triangleCountB);
                auto const minCount = std::min(triangleCountA, triangleCountB);
                double const relativeDifference = maxCount == 0U
                                                                                        ? 0.0
                                                                                        : static_cast<double>(maxCount - minCount) /
                                                                                                static_cast<double>(maxCount);

                EXPECT_LT(relativeDifference, 0.001)
                    << "Projection toggles should remain topologically similar";
    }

    struct ExtractionScenario
    {
        const char * name;
        bool enableCoarsening;
        bool enableGpu;
        float minFeatureSize;
        HierarchicalQuality quality;
        double maxDegenerateRatio;
    };

    class HierarchicalDC_ExtractionStepsParamTest : public HierarchicalDC_ExtractionStepsTest,
                                                    public ::testing::WithParamInterface<ExtractionScenario>
    {
    };

    TEST_P(HierarchicalDC_ExtractionStepsParamTest, ExtractMesh_CombinationsRemainStable)
    {
        auto const scenario = GetParam();
        SCOPED_TRACE(scenario.name);

        auto bundle = loadImplicitGyroid();
        auto const bbox = tryComputeBoundingBox(bundle);
        if (!bbox.has_value())
        {
            GTEST_SKIP() << "Unable to determine bounding box";
        }

        HierarchicalConfig config;
        applyQualityPreset(config, scenario.quality);
        config.enableCoarsening = scenario.enableCoarsening;
        config.enableGpuAcceleration = scenario.enableGpu;
        config.minFeatureSize = scenario.minFeatureSize;
        config.projectVerticesToSurface = false;

        HierarchicalOctreeBuilder builder(*bundle.core, config);
        builder.buildOctree(bbox.value());

        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::uint32_t> indices;
        builder.extractMesh(vertices, indices);

        ASSERT_FALSE(vertices.empty());
        ASSERT_FALSE(indices.empty());
        EXPECT_EQ(indices.size() % 3U, 0U);

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

        double const totalTriangles = static_cast<double>(indices.size() / 3U);
        double const degenerateRatio = totalTriangles == 0.0
                                         ? 0.0
                                         : static_cast<double>(degenerateCount) / totalTriangles;
                EXPECT_LE(degenerateRatio, scenario.maxDegenerateRatio)
                    << "Scenario " << scenario.name << " produced too many degenerate triangles";
    }

    constexpr ExtractionScenario kExtractionScenarios[] = {
                {"CPU_Balanced_Default", false, false, 0.0F, HierarchicalQuality::Balanced, 0.0},
                {"CPU_Balanced_MinFeature", false, false, 0.25F, HierarchicalQuality::Balanced, 0.0},
                {"CPU_Fine_Coarsening", true, false, 0.0F, HierarchicalQuality::Fine, 0.0},
                {"GPU_Balanced_Default", false, true, 0.0F, HierarchicalQuality::Balanced, 0.0},
                {"GPU_Balanced_MinFeature", false, true, 0.25F, HierarchicalQuality::Balanced, 0.0},
                {"GPU_Fine_Coarsening", true, true, 0.0F, HierarchicalQuality::Fine, 0.0},
    };

    INSTANTIATE_TEST_SUITE_P(
      HierarchicalDualContouringExtractionCombinations,
      HierarchicalDC_ExtractionStepsParamTest,
      ::testing::ValuesIn(kExtractionScenarios),
      [](testing::TestParamInfo<ExtractionScenario> const & info) {
          return std::string(info.param.name);
      });

} // namespace gladius_tests::hierarchical_dc_mesh
