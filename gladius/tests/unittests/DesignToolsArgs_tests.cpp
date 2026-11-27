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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    namespace
    {
        /// @brief Harness with the geometry-only tool surface and an optional finish/graph policy
        struct SurfaceHarness
        {
            DesignGraph graph;
            DesignCatalogue catalogue;
            tools::TransactionManager transactions;
            tools::CheckpointManager checkpoints;
            /// @brief Must outlive the registered tool lambdas, therefore a member
            DesignToolContext ctx;
            tools::ToolRegistry registry;
            tools::ToolExecutor executor;
            io::CancellationToken cancellation;
            std::size_t captures = 0;

            explicit SurfaceHarness(DesignToolSurface surface,
                                    bool autoCompleteOnFinish = false,
                                    std::size_t maxViewportReviews = 0)
                : ctx{graph,
                      catalogue,
                      transactions,
                      checkpoints,
                      nullptr,
                      [this](int, int, std::string const &)
                      {
                          ++captures;
                          return std::vector<std::byte>{std::byte{0x89}, std::byte{0x50},
                                                        std::byte{0x4e}, std::byte{0x47}};
                      },
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      autoCompleteOnFinish,
                      maxViewportReviews}
                , executor(registry)
            {
                registerDesignTools(registry, ctx, surface);
            }
        };
    } // namespace

    TEST(RegisterDesignTools_GeometryOnlySurface, Registry_KeepsOnlyAllowlistedTools)
    {
        // Arrange / Act
        SurfaceHarness full{DesignToolSurface::Full};
        SurfaceHarness geometry{DesignToolSurface::GeometryOnly};

        // Assert: the tool list is re-sent every turn, so the reduced surface must really be smaller
        EXPECT_GT(full.registry.definitions().size(), geometry.registry.definitions().size());
        auto const has = [&geometry](char const * const name)
        {
            auto const defs = geometry.registry.definitions();
            return std::any_of(defs.begin(), defs.end(),
                               [name](tools::ToolDefinition const & def) { return def.name == name; });
        };
        for (auto const * const kept : {"set_program_snippet", "get_program_snippet", "get_model_summary",
                                        "capture_viewport", "finish_design", "list_library_items",
                                        "import_library_item"})
        {
            EXPECT_TRUE(has(kept)) << kept << " missing from the geometry surface";
        }
        for (auto const * const dropped : {"create_todo_list", "update_todo_item", "store_knowledge",
                                          "create_variant", "create_configuration",
                                          "activate_configuration", "add_function", "hybridize_variants",
                                          "get_design_summary"})
        {
            EXPECT_FALSE(has(dropped)) << dropped << " should not be offered on the geometry surface";
        }
    }

    TEST(FinishDesign_GeometryOnlyWithoutBookkeeping, ToolCall_FailsAndNamesTheCeremony)
    {
        // Arrange: geometry applied, but no node/variant/configuration exists
        SurfaceHarness harness{DesignToolSurface::GeometryOnly};

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert
        EXPECT_FALSE(result.value("success", true));
        EXPECT_NE(result.value("error", "").find("create_configuration"), std::string::npos)
          << result.dump();
        EXPECT_EQ(harness.graph.activeConfiguration(), std::nullopt);
    }

    TEST(RegisterDesignTools_RoleSurfaces, Registry_KeepsOnlyAllowlistedTools)
    {
        // Arrange / Act
        SurfaceHarness requirements{DesignToolSurface::Requirements};
        SurfaceHarness decomposition{DesignToolSurface::Decomposition};
        SurfaceHarness component{DesignToolSurface::Component};
        SurfaceHarness review{DesignToolSurface::Review};
        auto const has = [](tools::ToolRegistry & registry, char const * const name)
        {
            return registry.hasTool(name);
        };
        auto const expectOrchestratorOnly = [&](tools::ToolRegistry & registry)
        {
            // Assert: the orchestrator alone owns finish_design and the bookkeeping ceremony
            EXPECT_FALSE(has(registry, "finish_design"));
            EXPECT_FALSE(has(registry, "create_configuration"));
            EXPECT_FALSE(has(registry, "store_knowledge"));
            EXPECT_LT(registry.definitions().size(), 20u) << "a role surface must stay cheap per turn";
        };

        expectOrchestratorOnly(requirements.registry);
        expectOrchestratorOnly(decomposition.registry);
        expectOrchestratorOnly(component.registry);
        expectOrchestratorOnly(review.registry);
        EXPECT_TRUE(has(requirements.registry, "add_requirement"));
        EXPECT_TRUE(has(requirements.registry, "create_todo_list"));
        EXPECT_FALSE(has(requirements.registry, "set_program_snippet"));

        EXPECT_TRUE(has(decomposition.registry, "add_function"));
        EXPECT_TRUE(has(decomposition.registry, "define_interface"));
        EXPECT_TRUE(has(decomposition.registry, "create_variant"));
        EXPECT_FALSE(has(decomposition.registry, "begin_edit"));

        EXPECT_TRUE(has(component.registry, "set_program_snippet"));
        EXPECT_TRUE(has(component.registry, "import_library_item"));
        EXPECT_TRUE(has(component.registry, "ensure_output_chain"));
        EXPECT_FALSE(has(component.registry, "create_variant"));

        EXPECT_TRUE(has(review.registry, "capture_viewport"));
        EXPECT_TRUE(has(review.registry, "reject_variant"));
        EXPECT_FALSE(has(review.registry, "commit_edit"));
    }

    TEST(RegisterDesignTools_RoleSurfaces, GeometrySurface_StaysAtItsRecordedSize)
    {
        // Arrange / Act: the baseline surface is what the existing benchmarks were recorded against
        SurfaceHarness geometry{DesignToolSurface::GeometryOnly};
        auto const names = geometry.registry.definitions();

        // Assert
        EXPECT_EQ(names.size(), 11u);
        EXPECT_TRUE(geometry.registry.hasTool("finish_design"));
        EXPECT_FALSE(geometry.registry.hasTool("get_node"));
    }

    TEST(FinishDesign_AutoCompleteEnabled, ToolCall_DerivesConfigurationAndCompletes)
    {
        // Arrange
        SurfaceHarness harness{DesignToolSurface::Full, /*autoCompleteOnFinish=*/true};

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert: the same validated end state without the four ceremony calls
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_TRUE(result.value("valid", false));
        EXPECT_EQ(harness.graph.phase(), DesignPhase::Complete);
        auto const active = harness.graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->selectedVariants.size(), harness.graph.nodes().size());
    }

    TEST(FinishDesign_AutoCompleteEnabledWithExistingSelections, ToolCall_KeepsThem)
    {
        // Arrange: a node and variant the model created itself
        SurfaceHarness harness{DesignToolSurface::Full, /*autoCompleteOnFinish=*/true};
        auto const nodeId = harness.graph.addNode(DesignNodeType::Component, "Body");
        auto const variantId = harness.graph.createVariant(nodeId, "Concept", "Manual", FidelityLevel::Envelope);

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        auto const active = harness.graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->selectedVariants.at(nodeId), variantId);
    }

    TEST(CaptureViewport_ReviewBudgetExhausted, ToolCall_FailsWithActionableError)
    {
        // Arrange
        SurfaceHarness harness{DesignToolSurface::GeometryOnly, false, /*maxViewportReviews=*/1};

        // Act
        auto const first = harness.executor.executeSync("capture_viewport", nlohmann::json::object(), harness.cancellation);
        auto const second = harness.executor.executeSync("capture_viewport", nlohmann::json::object(), harness.cancellation);

        // Assert
        EXPECT_TRUE(first.value("success", false)) << first.dump();
        EXPECT_FALSE(second.value("success", true));
        EXPECT_NE(second.value("error", "").find("finish_design"), std::string::npos) << second.dump();
        EXPECT_EQ(harness.captures, 1u) << "Exhausted budget must not render again";
    }
    TEST(AddTodoItem_ChecklistWithUnknownKind, ToolCall_RejectsInsteadOfDefaultingToManual)
    {
        ToolHarness harness;

        // Arrange / Act: a misspelled kind would silently become self-certifiable
        auto const result = harness.executor.executeSync(
          "add_todo_item",
          {{"title", "Wheel"}, {"checklist", {{{"label", "Renders"}, {"kind", "rednerability"}}}}},
          harness.cancellation);

        // Assert
        EXPECT_FALSE(result.value("success", true)) << result.dump();
        EXPECT_NE(result.value("error", "").find("rednerability"), std::string::npos) << result.dump();
        EXPECT_TRUE(harness.todos.items().empty());
    }

    TEST(AddTodoItem_ChecklistAsBareStrings, ToolCall_AcceptsTheLegacyShape)
    {
        ToolHarness harness;

        auto const result = harness.executor.executeSync(
          "add_todo_item", {{"title", "Wheel"}, {"checklist", {"Renders", "Fits the domain"}}},
          harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
        ASSERT_EQ(harness.todos.items().size(), 1u);
        EXPECT_EQ(harness.todos.items()[0].checklist.size(), 2u);
        EXPECT_EQ(harness.todos.items()[0].checklist[0].kind, ChecklistKind::Manual);
    }

    TEST(UpdateTodoItem_CheckIndexOnDeterministicEntry, ToolCall_RefusesTheSelfCheck)
    {
        ToolHarness harness;

        // Arrange: the model tries to certify its own geometry by flipping a flag
        auto const created = harness.executor.executeSync(
          "add_todo_item",
          {{"title", "Wheel"}, {"checklist", {{{"label", "Renders"}, {"kind", "renderability"}}}}},
          harness.cancellation);
        ASSERT_TRUE(created.value("success", false)) << created.dump();
        auto const todoId = created.at("todoId").get<std::string>();

        // Act
        auto const result = harness.executor.executeSync(
          "update_todo_item", {{"todoId", todoId}, {"status", "done"}, {"checkIndex", 0}}, harness.cancellation);

        // Assert: nothing is checked, and the error says who may check it
        EXPECT_FALSE(result.value("success", true)) << result.dump();
        EXPECT_NE(result.value("error", "").find("only by gates"), std::string::npos) << result.dump();
        EXPECT_FALSE(harness.todos.items()[0].checklist[0].checked);
    }

    TEST(ListLibraryItems_MultiWordQuery, ToolCall_MatchesAnyWordAndNotesEmptyResult)
    {
        // Arrange: a sandbox library whose item is named for one word of a natural phrase
        auto const root = std::filesystem::temp_directory_path() / "gladius_lib_query_test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "widgets");
        std::ofstream(root / "widgets" / "widget_alpha.3mf");
        ::setenv("GLADIUS_USER_LIBRARY_DIR", root.string().c_str(), 1);

        ToolHarness harness;

        // Act
        auto const phrase = harness.executor.executeSync(
          "list_library_items", {{"query", "alpha widget"}}, harness.cancellation);
        auto const miss = harness.executor.executeSync(
          "list_library_items", {{"query", "zzz-no-such-part"}}, harness.cancellation);
        ::unsetenv("GLADIUS_USER_LIBRARY_DIR");
        std::filesystem::remove_all(root);

        // Assert: requiring the whole phrase would return nothing and push the agent into
        // hand writing math for parts the library already provides
        EXPECT_TRUE(phrase.value("success", false)) << phrase.dump();
        EXPECT_GE(phrase.value("count", 0u), 1u) << phrase.dump();
        EXPECT_TRUE(miss.value("success", false)) << miss.dump();
        EXPECT_EQ(miss.value("count", 999u), 0u);
        EXPECT_FALSE(miss.value("note", "").empty()) << "Empty result must say how to search";
    }
} // namespace gladius::design::tests
