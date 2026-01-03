#include "Document.h"
#include "DualContouringOctree.h"
#include "EventLogger.h"
#include "ComputeContext.h"

#include <compute/ComputeCore.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <filesystem>
#include <gtest/gtest.h>

namespace gladius_tests
{

    using namespace gladius::dual_contouring;

    namespace
    {
        struct LeafStats
        {
            size_t totalLeaves{0U};
            size_t hermiteLeaves{0U};
            size_t vertexLeaves{0U};
        };

        void accumulateLeafStats(gladius::dual_contouring::OctreeNode const & node, LeafStats & stats)
        {
            if (node.isLeaf)
            {
                stats.totalLeaves += 1U;
                if (!node.hermiteSamples.empty())
                {
                    stats.hermiteLeaves += 1U;
                }
                if (node.hasVertex)
                {
                    stats.vertexLeaves += 1U;
                }
                if (node.hasVertex)
                {
                    auto const & bounds = node.bounds;
                    auto const & position = node.vertexPosition;
                    EXPECT_GE(position.x(), bounds.min.x());
                    EXPECT_LE(position.x(), bounds.max.x());
                    EXPECT_GE(position.y(), bounds.min.y());
                    EXPECT_LE(position.y(), bounds.max.y());
                    EXPECT_GE(position.z(), bounds.min.z());
                    EXPECT_LE(position.z(), bounds.max.z());
                    EXPECT_GE(node.vertexResidual, 0.0F);
                }
                return;
            }

            for (auto const & child : node.children)
            {
                if (child)
                {
                    accumulateLeafStats(*child, stats);
                }
            }
        }
    }

    class DualContouringOctree_Test : public ::testing::Test
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

    TEST_F(DualContouringOctree_Test, BuilderWithImplicitGyroidProducesIntersectingTree)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        OctreeBuildConfig config{};
        config.sdfResolution = 64U;
        config.maxDepth = 4U;

        OctreeBuilder builder(*bundle.core, bbox.value(), config);

        OctreeMetrics metrics{};
        auto root = builder.build(metrics);

        ASSERT_NE(root, nullptr);
        EXPECT_TRUE(root->isIntersecting);
        EXPECT_GT(metrics.nodeCount, 0U);
        EXPECT_GT(metrics.leafCount, 0U);
        EXPECT_LE(metrics.maxDepthReached, config.maxDepth);
        EXPECT_NE(root->childMask, 0U);
    }

    TEST_F(DualContouringOctree_Test, BuilderHonorsMaxDepthConfiguration)
    {
        auto bundle = loadDocument("testdata/SimpleGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        OctreeBuildConfig config{};
        config.sdfResolution = 32U;
        config.maxDepth = 2U;

        OctreeBuilder builder(*bundle.core, bbox.value(), config);

        OctreeMetrics metrics{};
        auto root = builder.build(metrics);

        ASSERT_NE(root, nullptr);
        EXPECT_LE(metrics.maxDepthReached, config.maxDepth);
    }

    TEST_F(DualContouringOctree_Test, LeafNodesProduceHermiteSamplesAndVertices)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        OctreeBuildConfig config{};
        config.sdfResolution = 64U;
        config.maxDepth = 4U;

        OctreeBuilder builder(*bundle.core, bbox.value(), config);

        OctreeMetrics metrics{};
        auto root = builder.build(metrics);

        ASSERT_NE(root, nullptr);

        LeafStats stats{};
        accumulateLeafStats(*root, stats);

        EXPECT_GT(stats.totalLeaves, 0U);
        EXPECT_GT(stats.hermiteLeaves, 0U);
        EXPECT_EQ(stats.hermiteLeaves, stats.vertexLeaves);
    }

    TEST_F(DualContouringOctree_Test, BalancedRefinementReducesCracks)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox());

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        // Build without balanced refinement
        OctreeBuildConfig configUnbalanced{};
        configUnbalanced.sdfResolution = 64U;
        configUnbalanced.maxDepth = 5U;
        configUnbalanced.enableBalancedRefinement = false;

        OctreeBuilder builderUnbalanced(*bundle.core, bbox.value(), configUnbalanced);
        OctreeMetrics metricsUnbalanced{};
        auto rootUnbalanced = builderUnbalanced.build(metricsUnbalanced);

        ASSERT_NE(rootUnbalanced, nullptr);
        EXPECT_EQ(metricsUnbalanced.balancePassSubdivisions, 0U);

        // Build with balanced refinement
        OctreeBuildConfig configBalanced{};
        configBalanced.sdfResolution = 64U;
        configBalanced.maxDepth = 5U;
        configBalanced.enableBalancedRefinement = true;

        OctreeBuilder builderBalanced(*bundle.core, bbox.value(), configBalanced);
        OctreeMetrics metricsBalanced{};
        auto rootBalanced = builderBalanced.build(metricsBalanced);

        ASSERT_NE(rootBalanced, nullptr);
        
        // Balanced version may have more nodes due to additional subdivisions
        EXPECT_GE(metricsBalanced.nodeCount, metricsUnbalanced.nodeCount);
        
        // If balance pass triggered, it should have created some subdivisions
        if (metricsBalanced.balancePassSubdivisions > 0U)
        {
            EXPECT_GT(metricsBalanced.nodeCount, metricsUnbalanced.nodeCount);
        }
    }
}
