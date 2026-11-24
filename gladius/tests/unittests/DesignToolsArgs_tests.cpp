#include "agent/KnowledgeStore.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/TodoStore.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/TransactionManager.h"
#include "io/CancellationToken.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace gladius::design::tests
{
    namespace
    {
        /// @brief Registers the design tools over an isolated context and returns the executor
        struct ToolHarness
        {
            DesignGraph graph;
            DesignCatalogue catalogue;
            tools::TransactionManager transactions;
            tools::CheckpointManager checkpoints;
            TodoStore todos;
            agent::KnowledgeStore knowledge;
            /// @brief Must outlive the registered tool lambdas, therefore a member
            DesignToolContext ctx;
            tools::ToolRegistry registry;
            tools::ToolExecutor executor;
            io::CancellationToken cancellation;

            ToolHarness()
                : knowledge(std::filesystem::temp_directory_path()
                            / "gladius_design_tools_args.json")
                , ctx{graph,
                      catalogue,
                      transactions,
                      checkpoints,
                      nullptr, // document: no geometry tools exercised here
                      nullptr, // captureViewport
                      nullptr, // validateModel
                      nullptr, // onDocumentModified
                      nullptr, // skills
                      &todos,
                      &knowledge}
                , executor(registry)
            {
                registerDesignTools(registry, ctx);
            }
        };
    } // namespace

    TEST(StoreKnowledge_SingleStringTags, ToolCall_SucceedsWithOneTag)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync("store_knowledge",
                                                        {{"title", "Gyroid walls"},
                                                         {"summary", "Wall thickness halves the SDF offset."},
                                                         {"tags", "gyroid"}},
                                                        harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_FALSE(result.value("knowledgeId", "").empty());
    }

    TEST(StoreKnowledge_ArrayTags, ToolCall_SucceedsWithSeveralTags)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync("store_knowledge",
                                                        {{"title", "Lego grid"},
                                                         {"summary", "Pitch is 8 mm."},
                                                         {"tags", {"lego", "grid"}}},
                                                        harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
    }

    TEST(StoreKnowledge_AbsentTags, ToolCall_SucceedsWithoutTags)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync("store_knowledge",
                                                        {{"title", "No tags"},
                                                         {"summary", "Tags are optional."}},
                                                        harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
    }

    TEST(UpdateKnowledge_SingleStringTags, ToolCall_StoresTagAsArray)
    {
        ToolHarness harness;

        auto const stored = harness.executor.executeSync("store_knowledge",
                                                         {{"title", "Initial"},
                                                          {"summary", "Initial summary"}},
                                                         harness.cancellation);
        ASSERT_TRUE(stored.value("success", false)) << stored.dump();

        auto const updated = harness.executor.executeSync("update_knowledge",
                                                          {{"id", stored.at("knowledgeId")},
                                                           {"title", "Updated"},
                                                           {"summary", "Updated summary"},
                                                           {"tags", "updated"}},
                                                          harness.cancellation);
        ASSERT_TRUE(updated.value("success", false)) << updated.dump();

        auto const found = harness.executor.executeSync("search_knowledge",
                                                        {{"query", "Updated"}},
                                                        harness.cancellation);
        ASSERT_TRUE(found.value("success", false)) << found.dump();
        ASSERT_EQ(found.at("entries").size(), 1u);
        EXPECT_EQ(found.at("entries")[0]["tags"], nlohmann::json::array({"updated"}));
    }

    TEST(CreateTodoList_SingleStringChecklist, ToolCall_AcceptsLoneChecklistItem)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync("create_todo_list",
                                                        {{"title", "Sphere"},
                                                         {"items",
                                                          nlohmann::json::array({{{"title", "Model"},
                                                                                  {"checklist", "set snippet"}}})}},
                                                        harness.cancellation);

        EXPECT_TRUE(result.value("success", false)) << result.dump();
    }

    TEST(ScoreComponentReusability_NumericScoresAsStrings, ToolCall_ClampsParsedScores)
    {
        // Arrange: a model that quotes the declared numbers ("0.9" instead of 0.9)
        ToolHarness harness;

        // Act
        auto const result = harness.executor.executeSync("score_component_reusability",
                                                        {{"componentName", "gyroidBall"},
                                                         {"qualityScore", "0.9"},
                                                         {"reusabilityScore", "0.8"}},
                                                        harness.cancellation);

        // Assert
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_DOUBLE_EQ(result.at("qualityScore").get<double>(), 0.9);
        EXPECT_TRUE(result.at("recommendLibraryExport").get<bool>());
    }

    TEST(ActivateConfiguration_UnknownId, ToolCall_ListsExistingIdsForOneShotRetry)
    {
        // Arrange
        ToolHarness harness;
        (void)harness.executor.executeSync("add_requirement",
                                          {{"description", "stiff wall"}},
                                          harness.cancellation);
        auto const node = harness.executor.executeSync("add_function",
                                                       {{"name", "Wall"}},
                                                       harness.cancellation);
        auto const variant = harness.executor.executeSync("create_variant",
                                                          {{"nodeId", node.value("nodeId", "")},
                                                           {"name", "gyroid"}},
                                                          harness.cancellation);
        auto const config = harness.executor.executeSync("create_configuration",
                                                         {{"name", "Wall config"},
                                                          {"selectedVariants",
                                                           {variant.value("variantId", "")}}},
                                                         harness.cancellation);
        ASSERT_TRUE(config.value("success", false)) << config.dump();

        // Act: the model guesses the id instead of using the one it was handed
        auto const result = harness.executor.executeSync("activate_configuration",
                                                        {{"configurationId", "config_1"}},
                                                        harness.cancellation);

        // Assert: a bare "unknown id" costs a whole extra turn to re-read the graph, so the error
        // has to carry what is actually available.
        ASSERT_FALSE(result.value("success", true));
        EXPECT_NE(result.value("error", "").find("Wall config ("), std::string::npos)
          << result.value("error", "");
        auto const retry = harness.executor.executeSync(
          "activate_configuration",
          {{"configurationId", config.value("configurationId", "")}},
          harness.cancellation);
        ASSERT_TRUE(retry.value("success", false)) << retry.dump();
    }

    TEST(CreateVariant_UnknownNodeId, ToolCall_ListsExistingNodes)
    {
        // Arrange
        ToolHarness harness;
        (void)harness.executor.executeSync("add_function", {{"name", "Wall"}},
                                          harness.cancellation);

        // Act
        auto const result = harness.executor.executeSync("create_variant",
                                                        {{"nodeId", "nope"}, {"name", "gyroid"}},
                                                        harness.cancellation);

        // Assert
        ASSERT_FALSE(result.value("success", true));
        EXPECT_NE(result.value("error", "").find("Wall ("), std::string::npos)
          << result.value("error", "");
    }

    TEST(GetDesignSummary_ToolCall, ReturnedWithoutInternalCountersOrPreviews)
    {
        // Arrange
        ToolHarness harness;
        (void)harness.executor.executeSync("add_requirement",
                                          {{"description", "3x4 brick footprint"}},
                                          harness.cancellation);

        // Act
        auto const result = harness.executor.executeSync("get_design_summary", {},
                                                        harness.cancellation);

        // Assert: the tool result rides along in history for every later turn, so the internal
        // bookkeeping fields must not be part of it
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_FALSE(result.contains("nextReqId"));
        EXPECT_FALSE(result.contains("nextNodeId"));
        EXPECT_FALSE(result.contains("nextVariantId"));
        EXPECT_FALSE(result.contains("nextConfigId"));
        EXPECT_NE(result.value("summary", "").find("Requirements: 1"), std::string::npos);
        EXPECT_EQ(result["requirements"].size(), 1u);
    }

    TEST(ScoreComponentReusability_UnparsableScore, ToolCall_FallsBackToZero)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync("score_component_reusability",
                                                        {{"componentName", "gyroidBall"},
                                                         {"qualityScore", "high"},
                                                         {"reusabilityScore", 0.9}},
                                                        harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_DOUBLE_EQ(result.at("qualityScore").get<double>(), 0.0);
        EXPECT_FALSE(result.at("recommendLibraryExport").get<bool>());
    }
} // namespace gladius::design::tests
