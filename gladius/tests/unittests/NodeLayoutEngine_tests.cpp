/**
 * @file NodeLayoutEngine_tests.cpp
 * @brief Unit tests for the NodeLayoutEngine class
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>


#include <algorithm>
#include <tuple>

#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

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
    static std::mutex & csvMutex()
    {
        static std::mutex m;
        return m;
    }

    /// Estimate node size based on number of inputs and outputs (mimics actual UI rendering)
    static ImVec2 estimateNodeSize(nodes::NodeBase const * node)
    {
        if (!node)
        {
            return ImVec2(200.0F, 140.0F);
        }

        // Base dimensions
        constexpr float baseWidth = 180.0F;
        constexpr float baseHeight = 80.0F;
        
        // Count inputs (parameters) and outputs
        auto const & params = node->constParameter();
        auto const & outputs = node->getOutputs();
        
        size_t numInputs = params.size();
        size_t numOutputs = outputs.size();
        
        // Width grows with more ports (need space for port labels)
        constexpr float widthPerPort = 15.0F;
        float width = baseWidth + widthPerPort * std::max(numInputs, numOutputs);
        
        // Height grows with number of rows (each port needs vertical space)
        constexpr float heightPerRow = 25.0F;
        float height = baseHeight + heightPerRow * std::max(numInputs, numOutputs);
        
        // Clamp to reasonable ranges
        width = std::clamp(width, 180.0F, 400.0F);
        height = std::clamp(height, 100.0F, 300.0F);
        
        return ImVec2(width, height);
    }

    static void appendCsvRow(std::string const & filePath,
                             std::vector<std::string> const & header,
                             std::vector<std::string> const & row)
    {
        std::lock_guard<std::mutex> lock(csvMutex());

        bool writeHeader = !std::filesystem::exists(filePath);

        std::ofstream out(filePath, std::ios::app);
        if (!out)
        {
            // Best effort: don't fail the test on I/O error.
            return;
        }

        auto writeLine = [&](std::vector<std::string> const & cols)
        {
            for (size_t i = 0; i < cols.size(); ++i)
            {
                out << cols[i];
                if (i + 1 < cols.size())
                {
                    out << ",";
                }
            }
            out << '\n';
        };

        if (writeHeader)
        {
            writeLine(header);
        }

        writeLine(row);
    }

    static float computeYScatter(nodes::Model & model, ImVec2 const nodeSize)
    {
        float minCenterY = std::numeric_limits<float>::infinity();
        float maxCenterY = -std::numeric_limits<float>::infinity();

        for (auto & pair : model)
        {
            auto & node = *pair.second;
            float const centerY = node.screenPos().y + nodeSize.y * 0.5f;
            minCenterY = std::min(minCenterY, centerY);
            maxCenterY = std::max(maxCenterY, centerY);
        }

        if (!std::isfinite(minCenterY) || !std::isfinite(maxCenterY))
        {
            return 0.0f;
        }

        return std::max(0.0f, maxCenterY - minCenterY);
    }
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

        // Baseline metrics capture current behaviour after improved spacing and transpose passes;
        // bounds enforce non-regression while allowing future improvements.
        constexpr float baselineWidth = 2060.0F;
        constexpr float baselineHeight = 1940.0F;
        constexpr float baselineLongestExtent = 2060.0F;
        constexpr float baselineArea = baselineWidth * baselineHeight; // 3,996,400
        constexpr float baselineSumEdgeLength = 3219.6F;
        constexpr float baselineMaxEdgeLength = 1092.9F;
        constexpr float extentTolerance = 6.0F;
        constexpr float lengthTolerance = 6.0F;
        constexpr float areaTolerance = 1500.0F;

        auto const longestExtent = std::max(metrics.width, metrics.height);

        EXPECT_GT(metrics.width, 0.0F);
        EXPECT_GT(metrics.height, 0.0F);
        EXPECT_GT(metrics.occupiedArea, 0.0F);
        EXPECT_LE(longestExtent, baselineLongestExtent + extentTolerance);
        EXPECT_LE(metrics.occupiedArea, baselineArea + areaTolerance);
        EXPECT_LE(metrics.sumEdgeLength, baselineSumEdgeLength + lengthTolerance);
        EXPECT_LE(metrics.maxEdgeLength, baselineMaxEdgeLength + lengthTolerance);
        EXPECT_EQ(metrics.edgeCount, 4U);
        EXPECT_EQ(metrics.edgeCrossings, 0U);
        EXPECT_TRUE(metrics.nodeOverlaps.empty());
        EXPECT_TRUE(metrics.groupOverlaps.empty());

        // Group metrics can vary per chosen strategy; ensure no overlaps and reasonable totals
        auto const computeMetricsIter = metrics.groupMetrics.find("Compute");
        ASSERT_NE(computeMetricsIter, metrics.groupMetrics.end());
        auto const & computeMetrics = computeMetricsIter->second;
        EXPECT_EQ(computeMetrics.nodeCount, 2U);
        EXPECT_EQ(computeMetrics.edgeCount, 1U);
        EXPECT_EQ(computeMetrics.edgeCrossings, 0U);
    }

    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_TournamentProducesChampion)
    {
        createRegressionGraph(*model);

        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 180.0F;
        config.layerSpacing = 420.0F;
        config.groupPadding = 120.0F;

        engine->performAutoLayout(*model, config);

        auto const & results = engine->getLastResults();
        ASSERT_GE(results.size(), 2U);

        std::size_t appliedCount = 0U;
        float bestScore = std::numeric_limits<float>::infinity();

        for (auto const & result : results)
        {
            EXPECT_FALSE(result.name.empty());

            if (result.applied)
            {
                ++appliedCount;
                bestScore = std::min(bestScore, result.score);
            }
            else
            {
                EXPECT_FALSE(std::isfinite(result.score));
            }
        }

        EXPECT_GT(appliedCount, 0U);

        auto const & championOpt = engine->getLastChampion();
        ASSERT_TRUE(championOpt.has_value());

        auto const & champion = championOpt.value();
        EXPECT_TRUE(champion.applied);
        EXPECT_NEAR(champion.score, bestScore, 1.0F);
        EXPECT_EQ(champion.metrics.edgeCrossings, 0U);
    }

    /**
     * @brief Even with zero padding parameters, layout must remain overlap-free.
     */
    TEST_F(NodeLayoutEngineTest, PerformAutoLayout_WithZeroPadding_AvoidsOverlaps)
    {
        // Use a slightly larger graph than trivial single node
        createRegressionGraph(*model);

        // Zero padding: algorithms must still prevent overlaps using node sizes
        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 0.0F;
        config.layerSpacing = 0.0F;
        config.groupPadding = 0.0F;

        // Use fixed realistic node size
        engine->setNodeSizeProvider([](nodes::NodeId) { return ImVec2(220.0F, 160.0F); });

        // Act
        engine->performAutoLayout(*model, config);

        // Assert: No node or group overlaps even without extra padding
        LayoutQualityAnalyzer analyzer([](nodes::NodeId) { return ImVec2(220.0F, 160.0F); });
        auto const metrics = analyzer.analyze(*model);

        EXPECT_TRUE(metrics.nodeOverlaps.empty());
        EXPECT_TRUE(metrics.groupOverlaps.empty());
        EXPECT_GT(metrics.width, 0.0F);
        EXPECT_GT(metrics.height, 0.0F);
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

        // Use realistic node size estimation based on inputs/outputs
        engine->setNodeSizeProvider([](nodes::NodeId nodeId) {
            // Try to find the node in all models
            // Note: This is a simplification since we don't have direct access to the model here
            // In practice, the app uses ed::GetNodeSize() which queries the actual rendered size
            return ImVec2(220.0F, 160.0F); // Fallback to reasonable average
        });

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

    // Parameterized benchmark test: compare strategies on Y-scatter and metrics
    struct StrategyParam
    {
        const char * name;
        NodeLayoutEngine::LayoutStrategy strategy;
    };

    class NodeLayoutEngineParamTest : public NodeLayoutEngineTest,
                                      public ::testing::WithParamInterface<StrategyParam>
    {
    };

    TEST_P(NodeLayoutEngineParamTest, PerformAutoLayout_YScatterBenchmark_ByStrategy)
    {
        // Load SphereInACage_small asset with multiple function graphs
        auto const assetPath = std::filesystem::path("testdata") / "SphereInACage_small.3mf";
        ASSERT_TRUE(std::filesystem::exists(assetPath)) << "Missing SphereInACage asset for benchmark.";

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!context->isValid())
        {
            GTEST_SKIP() << "OpenCL context not available on this test system.";
        }
        context->setLogger(logger);
        setGlobalLogger(logger);

        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);
        auto document = std::make_shared<Document>(core);

        ASSERT_NO_THROW(document->load(assetPath));

        auto assembly = document->getAssembly();
        ASSERT_NE(assembly, nullptr);

        auto & functions = assembly->getFunctions();
        ASSERT_FALSE(functions.empty()) << "No function graphs found in SphereInACage asset.";

        // Strategy under test
        auto const & param = GetParam();
        engine->clearCustomStrategies();
        engine->setStrategies({param.strategy});

        // Use realistic node size based on input/output count
        ImVec2 const testNodeSize = ImVec2(220.0F, 160.0F);
        std::shared_ptr<nodes::Model> currentModel;
        
        engine->setNodeSizeProvider([&](nodes::NodeId nodeId) -> ImVec2 {
            if (currentModel)
            {
                auto opt = currentModel->getNode(nodeId);
                if (opt.has_value() && opt.value() != nullptr)
                {
                    return estimateNodeSize(opt.value());
                }
            }
            return testNodeSize;
        });

        NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = 180.0F;
        config.layerSpacing = 420.0F;
        config.groupPadding = 120.0F;

        LayoutQualityAnalyzer analyzer([&](nodes::NodeId nodeId) -> ImVec2 {
            if (currentModel)
            {
                auto opt = currentModel->getNode(nodeId);
                if (opt.has_value() && opt.value() != nullptr)
                {
                    return estimateNodeSize(opt.value());
                }
            }
            return testNodeSize;
        });

        // CSV setup
        auto const csvPath = (std::filesystem::current_path() / "node_layout_bench.csv").string();
        std::vector<std::string> header{"graph",
                                         "strategy",
                                         "yScatter",
                                         "width",
                                         "height",
                                         "area",
                                         "sumEdgeLength",
                                         "maxEdgeLength",
                                         "edgeCount",
                                         "crossings"};

        auto toStrF = [](float v)
        {
            std::ostringstream os;
            os.setf(std::ios::fixed, std::ios::floatfield);
            os.precision(3);
            os << v;
            return os.str();
        };

        // Benchmark each function graph
        for (auto const & [resourceId, functionModel] : functions)
        {
            if (!functionModel)
            {
                continue;
            }

            functionModel->updateGraphAndOrderIfNeeded();
            
            // Set current model for size provider
            currentModel = functionModel;
            
            // Act
            engine->performAutoLayout(*functionModel, config);

            // Analyze
            auto const metrics = analyzer.analyze(*functionModel);
            float const yScatter = computeYScatter(*functionModel, testNodeSize);
            auto const graphName = ensureModelName(*functionModel, resourceId);

            // Log for bench collection
            std::cout.setf(std::ios::fixed, std::ios::floatfield);
            std::cout.precision(3);
            std::cout << "[YScatter] graph=" << graphName << " strategy=" << param.name 
                      << " scatter=" << yScatter
                      << " width=" << metrics.width << " height=" << metrics.height
                      << " area=" << metrics.occupiedArea
                      << " crossings=" << metrics.edgeCrossings << std::endl;

            // CSV export
            std::vector<std::string> row{graphName,
                                          param.name,
                                          toStrF(yScatter),
                                          toStrF(metrics.width),
                                          toStrF(metrics.height),
                                          toStrF(metrics.occupiedArea),
                                          toStrF(metrics.sumEdgeLength),
                                          toStrF(metrics.maxEdgeLength),
                                          std::to_string(metrics.edgeCount),
                                          std::to_string(metrics.edgeCrossings)};

            appendCsvRow(csvPath, header, row);

            // Assert invariants
            EXPECT_GT(metrics.width, 0.0F) << "Graph: " << graphName;
            EXPECT_GT(metrics.height, 0.0F) << "Graph: " << graphName;
            EXPECT_TRUE(metrics.nodeOverlaps.empty()) << "Graph: " << graphName;
            EXPECT_TRUE(metrics.groupOverlaps.empty()) << "Graph: " << graphName;
            EXPECT_GE(yScatter, 0.0F) << "Graph: " << graphName;
        }

        std::cout << "[CSV] wrote=" << csvPath << std::endl;
    }

    // Instantiate with the same strategies used in the app/editor
    INSTANTIATE_TEST_SUITE_P(
        Strategies,
        NodeLayoutEngineParamTest,
        ::testing::Values(
            StrategyParam{"OptimizedLayered Median",
                          NodeLayoutEngine::LayoutStrategy{"OptimizedLayered Median",
                                                           NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                           0.95F,
                                                           1.05F,
                                                           180,
                                                           0.8F,
                                                           true,
                                                           2,
                                                           true,
                                                           true}},
            StrategyParam{"LayeredStack Classic", NodeLayoutEngine::LayoutStrategy{"LayeredStack Classic"}},
            StrategyParam{"LayeredRow Sweep",
                          NodeLayoutEngine::LayoutStrategy{"LayeredRow Sweep",
                                                           NodeLayoutEngine::GroupLayoutMode::HorizontalRow,
                                                           1.0F,
                                                           1.1F,
                                                           120,
                                                           0.9F,
                                                           true,
                                                           1,
                                                           true,
                                                           true}},
            StrategyParam{"MedianSweep TightY",
                          NodeLayoutEngine::LayoutStrategy{"MedianSweep TightY",
                                                           NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                           1.0F,  // Full nodeDistance (was 0.9F)
                                                           1.0F,  // Full layerSpacing (was 0.9F)
                                                           140,
                                                           0.8F,
                                                           true,
                                                           3,
                                                           true,
                                                           true}},
            StrategyParam{"WeightedMedian Compact",
                          NodeLayoutEngine::LayoutStrategy{"WeightedMedian Compact",
                                                           NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                           1.0F,  // Full nodeDistance (was 0.88F)
                                                           1.0F,  // Full layerSpacing (was 0.88F)
                                                           150,
                                                           0.75F,
                                                           true,
                                                           4,
                                                           true,
                                                           true}},
            StrategyParam{"HybridLayered Tight",
                          NodeLayoutEngine::LayoutStrategy{"HybridLayered Tight",
                                                           NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                           1.0F,   // Full nodeDistance (was 0.92F)
                                                           1.05F,  // More layer spacing (was 0.9F)
                                                           135,
                                                           0.82F,
                                                           true,
                                                           3,
                                                           true,
                                                           true}},
            StrategyParam{"BalancedGrid Compact",
                          NodeLayoutEngine::LayoutStrategy{"BalancedGrid Compact",
                                                           NodeLayoutEngine::GroupLayoutMode::BalancedGrid,
                                                           1.0F,   // Full nodeDistance (was 0.9F)
                                                           0.95F,  // Slightly tighter layer spacing (was 0.85F)
                                                           140,
                                                           0.85F,
                                                           true,
                                                           2,
                                                           true,
                                                           true}},
            StrategyParam{"ForceRefined Hybrid",
                          NodeLayoutEngine::LayoutStrategy{"ForceRefined Hybrid",
                                                           NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                           1.0F,   // Full nodeDistance (was 0.95F)
                                                           1.05F,  // More layer spacing (was 1.0F)
                                                           160,
                                                           0.7F,
                                                           true,
                                                           3,
                                                           true,
                                                           true}}));

} // namespace gladius::ui::tests
