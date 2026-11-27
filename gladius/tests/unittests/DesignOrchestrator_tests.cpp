#include "Document.h"
#include "EventLogger.h"
#include "agent/DesignOrchestrator.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/ScratchDocument.h"
#include "design/TemplateSeed.h"
#include "design/TodoStore.h"
#include "io/CancellationToken.h"
#include "nodes/Assembly.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/ToolRegistry.h"
#include "tools/TransactionManager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gladius::agent::tests
{
    namespace
    {
        LeafExecutionOutcome makePassingOutcome(std::string const & nodeId,
                                                std::string const & fnName = "")
        {
            LeafExecutionOutcome outcome;
            outcome.loopCompleted = true;
            outcome.modelTurns = 2;
            outcome.usage.inputTokens = 100;
            outcome.usage.outputTokens = 40;
            outcome.hasReport = true;
            outcome.report.nodeId = nodeId;
            outcome.report.satisfied = {"envelope", "interfaces"};
            outcome.scratchValidation.passed = true;
            outcome.scratchValidation.flatNodeCount = 12;
            outcome.importedFunctionName = fnName;
            return outcome;
        }
    } // namespace

    TEST(DesignOrchestrator_Dependencies, DispatchesInDependencyOrderAndFreezes)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        auto const tyreId = graph.addNode(design::DesignNodeType::Component, "Tyre");
        auto const rimId = graph.addNode(design::DesignNodeType::Component, "Rim");

        DesignOrchestrator orchestrator(graph, todos, nullptr);
        orchestrator.setLeafRunner(
          [](ComponentTask const & task,
             std::size_t,
             std::string const &,
             design::DesignGraph &,
             design::TodoStore &,
             io::CancellationToken const &)
          { return makePassingOutcome(task.nodeId); });

        // Schedule Rim before Tyre even though Rim depends on Tyre.
        std::vector<ComponentTask> const tasks{
          ComponentTask{.nodeId = rimId, .dependsOn = {tyreId}},
          ComponentTask{.nodeId = tyreId, .dependsOn = {}}};

        OrchestratorConfig cfg;
        cfg.composeIntoTargetDocument = false;
        io::CancellationToken cancellation;

        auto const result = orchestrator.run(tasks, cfg, cancellation);

        ASSERT_TRUE(result.completed) << result.error;
        ASSERT_EQ(result.executionOrder.size(), 2u);
        EXPECT_EQ(result.executionOrder[0], tyreId);
        EXPECT_EQ(result.executionOrder[1], rimId);
        EXPECT_TRUE(graph.isNodeFrozen(tyreId));
        EXPECT_TRUE(graph.isNodeFrozen(rimId));
        EXPECT_EQ(result.subAgentRuns, 2u);
        EXPECT_EQ(result.redesigns, 0u);
        EXPECT_EQ(result.perRoleTurns.at("component"), 4u);
    }

    TEST(DesignOrchestrator_MissingReport, EscalatesAfterRedesignBudget)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        auto const tyreId = graph.addNode(design::DesignNodeType::Component, "Tyre");

        DesignOrchestrator orchestrator(graph, todos, nullptr);
        orchestrator.setLeafRunner(
          [](ComponentTask const &,
             std::size_t,
             std::string const &,
             design::DesignGraph &,
             design::TodoStore &,
             io::CancellationToken const &)
          {
              LeafExecutionOutcome outcome;
              outcome.loopCompleted = true;
              outcome.hasReport = false; // Missing report_component => treated as failure
              return outcome;
          });

        OrchestratorConfig cfg;
        cfg.composeIntoTargetDocument = false;
        cfg.maxRedesignsPerNode = 2;
        io::CancellationToken cancellation;

        auto const result =
          orchestrator.run({ComponentTask{.nodeId = tyreId}}, cfg, cancellation);

        EXPECT_FALSE(result.completed);
        EXPECT_FALSE(result.error.empty());
        EXPECT_EQ(result.subAgentRuns, 3u);
        EXPECT_FALSE(graph.isNodeFrozen(tyreId));
    }

    TEST(DesignOrchestrator_RetrySuccess, RedesignedLeafPassesAndFreezes)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        auto const tyreId = graph.addNode(design::DesignNodeType::Component, "Tyre");

        DesignOrchestrator orchestrator(graph, todos, nullptr);
        orchestrator.setLeafRunner(
          [](ComponentTask const & task,
             std::size_t attempt,
             std::string const & feedback,
             design::DesignGraph &,
             design::TodoStore &,
             io::CancellationToken const &)
          {
              if (attempt == 0)
              {
                  LeafExecutionOutcome bad;
                  bad.hasReport = true;
                  bad.report.nodeId = task.nodeId;
                  bad.report.unsatisfied = {"wall thickness < 1mm"};
                  return bad;
              }
              EXPECT_FALSE(feedback.empty());
              return makePassingOutcome(task.nodeId);
          });

        OrchestratorConfig cfg;
        cfg.composeIntoTargetDocument = false;
        io::CancellationToken cancellation;

        auto const result =
          orchestrator.run({ComponentTask{.nodeId = tyreId}}, cfg, cancellation);

        ASSERT_TRUE(result.completed) << result.error;
        EXPECT_EQ(result.redesigns, 1u);
        EXPECT_EQ(result.subAgentRuns, 2u);
        EXPECT_TRUE(graph.isNodeFrozen(tyreId));
    }

    TEST(DesignOrchestrator_RequirementRevision, UnfreezesAffectedNodeAndDependents)
    {
        design::DesignGraph graph;
        design::TodoStore todos;
        auto const reqId = graph.addRequirement("Outer diameter 40 mm", "dimensional", true);
        auto const tyreId =
          graph.addNode(design::DesignNodeType::Component, "Tyre", "", {reqId});
        auto const rimId = graph.addNode(design::DesignNodeType::Component, "Rim");
        graph.freezeNode(tyreId);
        graph.freezeNode(rimId);

        std::vector<ComponentTask> const tasks{
          ComponentTask{.nodeId = tyreId},
          ComponentTask{.nodeId = rimId, .dependsOn = {tyreId}}};
        std::vector<std::string> pending;
        std::map<std::string, bool> frozen{{tyreId, true}, {rimId, true}};

        DesignOrchestrator orchestrator(graph, todos, nullptr);
        orchestrator.reviseRequirementAndRequeue(
          reqId, "Outer diameter 44 mm", tasks, pending, frozen);

        EXPECT_FALSE(graph.isNodeFrozen(tyreId));
        EXPECT_FALSE(graph.isNodeFrozen(rimId));
        EXPECT_FALSE(frozen[tyreId]);
        EXPECT_FALSE(frozen[rimId]);
        EXPECT_EQ(pending.size(), 2u);
        EXPECT_TRUE(graph.hasPendingRequirementChanges());
    }

    TEST(ScratchComponentDocument_MergeAndCompose, BuildsValidMultiComponentAssembly)
    {
        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document targetDoc(logger);
        auto const seed = design::seedVolumetricTemplate(&targetDoc);
        ASSERT_TRUE(seed.success) << seed.error;

        design::ScratchComponentDocument scratchA(logger, nullptr);
        design::ScratchComponentDocument scratchB(logger, nullptr);
        std::string err;
        ASSERT_TRUE(scratchA.initialize(err)) << err;
        ASSERT_TRUE(scratchB.initialize(err)) << err;

        auto const valA = scratchA.validateStandalone();
        auto const valB = scratchB.validateStandalone();
        ASSERT_TRUE(valA.passed) << valA.error;
        ASSERT_TRUE(valB.passed) << valB.error;

        std::string fnA;
        std::string fnB;
        ASSERT_TRUE(scratchA.mergeInto(targetDoc, "", fnA, err)) << err;
        ASSERT_TRUE(scratchB.mergeInto(targetDoc, "", fnB, err)) << err;
        EXPECT_FALSE(fnA.empty());
        EXPECT_FALSE(fnB.empty());
        EXPECT_NE(fnA, fnB);

        ASSERT_TRUE(design::composeMain3Union(targetDoc, {fnA, fnB}, err)) << err;
        auto const facts = design::collectRenderabilityFacts(&targetDoc);
        EXPECT_TRUE(facts.isRenderable()) << facts.blockersAsText();
    }

    TEST(AddFunction_DepthGuard, RequiresJustificationAtDepth4AndRefusesAtDepth6)
    {
        design::DesignGraph graph;
        design::DesignCatalogue catalogue;
        tools::TransactionManager transactions;
        tools::CheckpointManager checkpoints;
        design::TodoStore todos;
        KnowledgeStore knowledge(
          std::filesystem::temp_directory_path() / "gladius_depth_guard_test.json");
        design::DesignToolContext ctx{
          graph, catalogue, transactions, checkpoints, nullptr, nullptr, nullptr, nullptr,
          nullptr, &todos, &knowledge};
        tools::ToolRegistry registry;
        design::registerDesignTools(registry, ctx, design::DesignToolSurface::Decomposition);
        tools::ToolExecutor executor(registry);
        io::CancellationToken cancellation;

        std::string parentId;
        for (int d = 0; d < 4; ++d)
        {
            auto const res = executor.executeSync(
              "add_function",
              {{"name", "Node_" + std::to_string(d)}, {"parentNodeId", parentId}},
              cancellation);
            ASSERT_TRUE(res.value("success", false)) << res.dump();
            parentId = res.value("nodeId", "");
        }

        // Depth 4 without justification -> rejected.
        auto const noJust = executor.executeSync(
          "add_function",
          {{"name", "Deep_4"}, {"parentNodeId", parentId}},
          cancellation);
        EXPECT_FALSE(noJust.value("success", true));

        // Depth 4 with justification -> accepted.
        auto const withJust = executor.executeSync(
          "add_function",
          {{"name", "Deep_4"}, {"parentNodeId", parentId}, {"justification", "Sub-seal"}},
          cancellation);
        ASSERT_TRUE(withJust.value("success", false)) << withJust.dump();
        std::string const depth4Id = withJust.value("nodeId", "");

        // Depth 5 with justification -> accepted.
        auto const depth5 = executor.executeSync(
          "add_function",
          {{"name", "Deep_5"}, {"parentNodeId", depth4Id}, {"justification", "Lip detail"}},
          cancellation);
        ASSERT_TRUE(depth5.value("success", false)) << depth5.dump();
        std::string const depth5Id = depth5.value("nodeId", "");

        // Depth 6 -> hard refused even with justification.
        auto const depth6 = executor.executeSync(
          "add_function",
          {{"name", "Deep_6"}, {"parentNodeId", depth5Id}, {"justification", "Too deep"}},
          cancellation);
        EXPECT_FALSE(depth6.value("success", true));
    }
} // namespace gladius::agent::tests
