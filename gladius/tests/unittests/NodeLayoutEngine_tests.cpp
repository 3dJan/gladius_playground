/**
 * @file NodeLayoutEngine_tests.cpp
 * @brief Unit tests for the NodeLayoutEngine class
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <tuple>

#include <filesystem>
#include <iostream>
#include <map>

#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"
#include "nodes/Assembly.h"
#include <compute/ComputeCore.h>
#include "ui/LayoutQualityAnalyzer.h"
#include "ui/NodeLayoutEngine.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"
#include "nodes/DerivedNodes.h"
#include "nodes/types.h"

namespace gladius::ui::tests
{
    class NodeLayoutEngineTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            engine = std::make_unique<NodeLayoutEngine>();
            engine->setNodeSizeProvider([](nodes::NodeId) { return ImVec2(200.0F, 140.0F); });
            model = std::make_shared<nodes::Model>();
            model->createBeginEndWithDefaultInAndOuts();
        }

        void TearDown() override
        {
            engine.reset();
            model.reset();
        }

        std::unique_ptr<NodeLayoutEngine> engine;
        std::shared_ptr<nodes::Model> model;
    };

    /**
     * @brief Test that performAutoLayout doesn't crash with empty model
     */
    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_WithEmptyModel_DoesNotCrash)
    {
        NodeLayoutEngine::LayoutConfig config;
        
        EXPECT_NO_THROW(engine->performAutoLayout(*model, config));
    }

    /**
     * @brief Test that performAutoLayout works with single node
     */
    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_WithSingleNode_PositionsNodeCorrectly)
    {
        // Arrange
        auto node = model->create<nodes::ConstantScalar>();
        node->screenPos() = gladius::nodes::float2{0, 0};
        
        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 100.0f;
        
        // Act
        EXPECT_NO_THROW(engine->performAutoLayout(*model, config));
        
        // Assert - Node should be positioned (exact position depends on implementation details)
        auto pos = node->screenPos();
        EXPECT_GE(pos.x, 0.0f);
        EXPECT_GE(pos.y, 0.0f);
    }


    /**
     * @brief Test edge case with very large nodeDistance
     */
    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_WithLargeNodeDistance_DoesNotOverflow)
    {
        // Arrange
        auto node = model->create<nodes::ConstantScalar>();
        node->screenPos() = gladius::nodes::float2{0, 0};
        
        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 10000.0f;
        
        // Act & Assert
        EXPECT_NO_THROW(engine->performAutoLayout(*model, config));
        
        // Position should be finite
        auto pos = node->screenPos();
        EXPECT_TRUE(std::isfinite(pos.x));
        EXPECT_TRUE(std::isfinite(pos.y));
    }

    /**
     * @brief Test edge case with zero nodeDistance
     */
    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_WithZeroNodeDistance_DoesNotCrash)
    {
        // Arrange
        auto node1 = model->create<nodes::ConstantScalar>();
        auto node2 = model->create<nodes::ConstantScalar>();

        node1->screenPos() = gladius::nodes::float2{0, 0};
        node2->screenPos() = gladius::nodes::float2{0, 0};

        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 0.0f;

        // Act & Assert
        EXPECT_NO_THROW(engine->performAutoLayout(*model, config));
    }

namespace
{
    void printMetrics(std::string const & label, LayoutQualityAnalyzer::Metrics const & metrics)
    {
        auto const previousFlags = std::cout.flags();
        auto const previousPrecision = std::cout.precision();

        std::cout.setf(std::ios::fixed, std::ios::floatfield);
        std::cout.precision(3);

        std::cout << "[LayoutMetrics] " << label << ": "
                  << "width=" << metrics.width << ", height=" << metrics.height
                  << ", occupiedArea=" << metrics.occupiedArea
                  << ", sumEdgeLength=" << metrics.sumEdgeLength
                  << ", maxEdgeLength=" << metrics.maxEdgeLength
                  << ", edgeCount=" << metrics.edgeCount
                  << ", edgeCrossings=" << metrics.edgeCrossings << std::endl;

        if (!metrics.groupMetrics.empty())
        {
            std::cout << "  Groups (" << metrics.groupMetrics.size() << "):" << std::endl;
            for (auto const & [groupName, groupMetrics] : metrics.groupMetrics)
            {
                std::cout << "    [" << groupName << "] width=" << groupMetrics.width
                          << ", height=" << groupMetrics.height
                          << ", occupiedArea=" << groupMetrics.occupiedArea
                          << ", sumEdgeLength=" << groupMetrics.sumEdgeLength
                          << ", maxEdgeLength=" << groupMetrics.maxEdgeLength
                          << ", edgeCount=" << groupMetrics.edgeCount
                          << ", edgeCrossings=" << groupMetrics.edgeCrossings
                          << ", nodeCount=" << groupMetrics.nodeCount << std::endl;
            }
        }

        if (!metrics.nodeOverlaps.empty())
        {
            std::cout << "  Node overlaps (" << metrics.nodeOverlaps.size() << "):";
            for (auto const & overlap : metrics.nodeOverlaps)
            {
                std::cout << " (" << overlap.first << "," << overlap.second << ")";
            }
            std::cout << std::endl;
        }

        if (!metrics.groupOverlaps.empty())
        {
            std::cout << "  Group overlaps (" << metrics.groupOverlaps.size() << "):";
            for (auto const & overlap : metrics.groupOverlaps)
            {
                std::cout << " (" << overlap.first << "," << overlap.second << ")";
            }
            std::cout << std::endl;
        }

        std::cout.flags(previousFlags);
        std::cout.precision(previousPrecision);
        std::cout << std::flush;
    }

    void createRegressionGraph(nodes::Model & model)
    {
        auto constA = model.create<nodes::ConstantScalar>();
        auto constB = model.create<nodes::ConstantScalar>();
        auto add = model.create<nodes::Addition>();
        auto constC = model.create<nodes::ConstantScalar>();
        auto multiply = model.create<nodes::Multiplication>();

        constA->setTag("Inputs");
        constB->setTag("Inputs");
        add->setTag("Compute");
        constC->setTag("Constants");
        multiply->setTag("Compute");

        auto const linkAB = model.addLink(constA->getValueOutputPort().getId(),
                                          add->parameter()[nodes::FieldNames::A].getId());
        auto const linkBB = model.addLink(constB->getValueOutputPort().getId(),
                                          add->parameter()[nodes::FieldNames::B].getId());

        auto const linkAddMultiply = model.addLink(add->getResultOutputPort().getId(),
                                                   multiply->parameter()[nodes::FieldNames::A].getId());
        auto const linkCMultiply = model.addLink(constC->getValueOutputPort().getId(),
                                                 multiply->parameter()[nodes::FieldNames::B].getId());

        EXPECT_TRUE(linkAB);
        EXPECT_TRUE(linkBB);
        EXPECT_TRUE(linkAddMultiply);
        EXPECT_TRUE(linkCMultiply);
    }

    std::string ensureModelName(nodes::Model & model, nodes::ResourceId resourceId)
    {
        auto & name = model.getModelName();
        if (name.empty())
        {
            return "resource_" + std::to_string(resourceId);
        }

        return name;
    }
} // namespace

    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_RegressionMetricsRemainStable)
    {
        createRegressionGraph(*model);

        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 180.0F;
        config.layerSpacing = 420.0F;
        config.groupPadding = 120.0F;

        engine->performAutoLayout(*model, config);

        LayoutQualityAnalyzer analyzer([](nodes::NodeId) { return ImVec2(200.0F, 140.0F); });
        auto const metrics = analyzer.analyze(*model);

        printMetrics("RegressionGraph", metrics);

        ASSERT_FALSE(metrics.groupMetrics.empty());

        EXPECT_NEAR(metrics.width, 820.0F, 5.0F);
        EXPECT_NEAR(metrics.height, 1832.0F, 5.0F);
        EXPECT_NEAR(metrics.sumEdgeLength, 2110.0F, 5.0F);
        EXPECT_NEAR(metrics.maxEdgeLength, 792.0F, 5.0F);
        EXPECT_EQ(metrics.edgeCount, 4U);
        EXPECT_EQ(metrics.edgeCrossings, 0U);
        EXPECT_TRUE(metrics.nodeOverlaps.empty());
        EXPECT_TRUE(metrics.groupOverlaps.empty());

        auto const computeMetricsIter = metrics.groupMetrics.find("Compute");
        ASSERT_NE(computeMetricsIter, metrics.groupMetrics.end());
        auto const & computeMetrics = computeMetricsIter->second;
        EXPECT_EQ(computeMetrics.nodeCount, 2U);
        EXPECT_NEAR(computeMetrics.width, 200.0F, 5.0F);
        EXPECT_NEAR(computeMetrics.height, 406.0F, 5.0F);
        EXPECT_NEAR(computeMetrics.sumEdgeLength, 266.0F, 5.0F);
        EXPECT_EQ(computeMetrics.edgeCount, 1U);
        EXPECT_EQ(computeMetrics.edgeCrossings, 0U);
    }

    TEST_F(NodeLayoutEngineTest,
           PerformAutoLayout_SphereInACageSmallModels_LayoutMetricsRemainStable)
    {
        auto const assetPath = std::filesystem::path("testdata") / "SphereInACage_small.3mf";
        ASSERT_TRUE(std::filesystem::exists(assetPath)) << "Missing SphereInACage asset for test.";

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!context->isValid())
        {
            GTEST_SKIP() << "OpenCL context not available on this test system.";
        }
        context->setLogger(logger);
        setGlobalLogger(logger);

        auto core =
            std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);
        auto document = std::make_shared<Document>(core);

        ASSERT_NO_THROW(document->load(assetPath));

        auto assembly = document->getAssembly();
        ASSERT_NE(assembly, nullptr);

        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 180.0F;
        config.layerSpacing = 420.0F;
        config.groupPadding = 120.0F;

        engine->setNodeSizeProvider([](nodes::NodeId) { return ImVec2(220.0F, 160.0F); });

        LayoutQualityAnalyzer analyzer([](nodes::NodeId) { return ImVec2(220.0F, 160.0F); });

        std::map<std::string, LayoutQualityAnalyzer::Metrics> metricsByModel;

        auto & functions = assembly->getFunctions();
        ASSERT_FALSE(functions.empty());

        for (auto const & [resourceId, functionModel] : functions)
        {
            if (!functionModel)
            {
                continue;
            }

            functionModel->updateGraphAndOrderIfNeeded();
            engine->performAutoLayout(*functionModel, config);
            auto metrics = analyzer.analyze(*functionModel);
            auto name = ensureModelName(*functionModel, resourceId);

            printMetrics(name, metrics);

            metricsByModel.insert_or_assign(std::move(name), std::move(metrics));
        }

        ASSERT_FALSE(metricsByModel.empty());

        for (auto const & [name, metrics] : metricsByModel)
        {
            SCOPED_TRACE("Metrics for model: " + name);
            EXPECT_TRUE(metrics.nodeOverlaps.empty());
            EXPECT_TRUE(metrics.groupOverlaps.empty());
            EXPECT_GT(metrics.width, 0.0F);
            EXPECT_GT(metrics.height, 0.0F);
        }
    }

} // namespace gladius::ui::tests
