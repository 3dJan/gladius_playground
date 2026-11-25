/**
 * @file DesignToolsRenderability_tests.cpp
 * @brief Tests for the renderability facts the design tools feed back into the agent loop.
 */

#include "Document.h"
#include "EventLogger.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/Renderability.h"
#include "design/TemplateSeed.h"
#include "io/CancellationToken.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/TransactionManager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace gladius::design::tests
{
    namespace
    {
        /// @brief Harness with a real (coreless) document and the geometry-only tool surface
        struct DocumentHarness
        {
            events::SharedLogger logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
            std::unique_ptr<Document> document = std::make_unique<Document>(logger);
            DesignGraph graph;
            DesignCatalogue catalogue;
            tools::TransactionManager transactions;
            tools::CheckpointManager checkpoints;
            /// @brief Must outlive the registered tool lambdas, therefore a member
            DesignToolContext ctx;
            tools::ToolRegistry registry;
            tools::ToolExecutor executor;
            io::CancellationToken cancellation;

            explicit DocumentHarness(bool autoCompleteOnFinish = true)
                : ctx{graph,
                      catalogue,
                      transactions,
                      checkpoints,
                      document.get(),
                      [](int, int, std::string const &)
                      {
                          return std::vector<std::byte>{std::byte{0x89}, std::byte{0x50},
                                                        std::byte{0x4e}, std::byte{0x47}};
                      },
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      autoCompleteOnFinish,
                      0}
                , executor(registry)
            {
                document->newModel();
                registerDesignTools(registry, ctx, DesignToolSurface::GeometryOnly);
            }

            /// @brief Replaces the blank document with the volumetric project template the app seeds
            void seedTemplate()
            {
                auto const seed = design::seedVolumetricTemplate(document.get());
                ASSERT_TRUE(seed.success) << seed.error;
            }

            /// @brief Adds a shape function the way a snippet does: as an assembly model whose
            /// output sink is sourced. The 3MF function only appears once the document is synced.
            ResourceId addShapeFunction(std::string const & name, ResourceId id)
            {
                auto const assembly = document->getAssembly();
                EXPECT_TRUE(assembly->addModelIfNotExisting(id));
                auto const model = assembly->findModel(id);
                EXPECT_NE(model, nullptr);
                model->createValidVoid();
                model->setDisplayName(name);
                return id;
            }
        };
    } // namespace

    TEST(FinishDesign_BlankDocument, ToolCall_FailsWithRenderabilityBlockers)
    {
        // Arrange: nothing but the valid-void scaffold, so the design cannot render
        DocumentHarness harness;

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert: the doc is checked before the graph bookkeeping, so the failure names the scene
        EXPECT_FALSE(result.value("success", true));
        EXPECT_NE(result.value("error", "").find("does not render"), std::string::npos)
          << result.dump();
        ASSERT_TRUE(result.contains("renderabilityBlockers"));
        EXPECT_GE(result["renderabilityBlockers"].size(), 2u);
        // The scene gate answers before any graph bookkeeping happens, so a rejected finish must
        // not leave derived graph state behind.
        EXPECT_FALSE(harness.graph.activeConfiguration().has_value());
    }

    TEST(EnsureOutputChain_FunctionWithoutOutputChain, ToolCall_MakesTheModelRenderable)
    {
        // Arrange
        DocumentHarness harness;
        auto const functionId = harness.addShapeFunction("main", 3);

        // Act
        auto const result = harness.executor.executeSync(
          "ensure_output_chain", {{"functionId", static_cast<int>(functionId)}},
          harness.cancellation);

        // Assert
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_TRUE(result.value("isRenderable", false));
        EXPECT_EQ(result.value("channel", ""), "shape");
    }

    TEST(EnsureOutputChain_UnknownFunctionId, ToolCall_NamesTheMissingFunction)
    {
        DocumentHarness harness;

        auto const result = harness.executor.executeSync(
          "ensure_output_chain", {{"functionId", 4242}}, harness.cancellation);

        EXPECT_FALSE(result.value("success", false));
        EXPECT_NE(result.value("error", "").find("4242"), std::string::npos) << result.dump();
    }

    TEST(EnsureOutputChain_MultipleCandidateFunctions, ToolCall_NamesTheMissingFunctionId)
    {
        // Arrange: two candidate functions, so guessing would be a coin flip
        DocumentHarness harness;
        harness.addShapeFunction("first", 3);
        harness.addShapeFunction("second", 4);

        // Act
        auto const result = harness.executor.executeSync("ensure_output_chain",
                                                         nlohmann::json::object(),
                                                         harness.cancellation);

        // Assert
        EXPECT_FALSE(result.value("success", false));
        EXPECT_NE(result.value("error", "").find("functionId"), std::string::npos) << result.dump();
    }

    TEST(FinishDesign_RepairedDocument, ToolCall_Completes)
    {
        // Arrange
        DocumentHarness harness;
        auto const functionId = harness.addShapeFunction("main", 3);
        auto const repair = harness.executor.executeSync(
          "ensure_output_chain", {{"functionId", static_cast<int>(functionId)}},
          harness.cancellation);
        ASSERT_TRUE(repair.value("success", false)) << repair.dump();

        // Act
        auto const result = harness.executor.executeSync("finish_design", {}, harness.cancellation);

        // Assert
        EXPECT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_TRUE(result.value("valid", false));
        EXPECT_EQ(toString(harness.graph.phase()), "Complete");
    }

    TEST(EnsureOutputChain_SeededTemplate, ToolCall_ReusesTheExistingChainInsteadOfAddingOne)
    {
        // Arrange: the template already has level set + domain + build item
        DocumentHarness harness;
        harness.seedTemplate();
        auto const before = collectRenderabilityFacts(harness.document.get());
        ASSERT_EQ(before.levelSetCount, 1);
        ASSERT_EQ(before.buildItemCount, 1);

        // Act: the agent repairs a scene that is already fine (it happens after a failed attempt)
        auto const result = harness.executor.executeSync(
          "ensure_output_chain",
          {{"functionId", static_cast<int>(before.rootFunctionId)}},
          harness.cancellation);

        // Assert: the second chain that used to double the geometry is never added
        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_EQ(result.value("chainAction", ""), "already-wired");
        auto const after = collectRenderabilityFacts(harness.document.get());
        EXPECT_EQ(after.levelSetCount, before.levelSetCount);
        EXPECT_EQ(after.buildItemCount, before.buildItemCount);
    }

    TEST(EnsureOutputChain_FunctionWithoutShapeOutput, ToolCall_NamesTheMissingPort)
    {
        // Arrange: a root that only returns a colour, the way a mistaken snippet often reads
        DocumentHarness harness;
        (void)harness.executor.executeSync("begin_edit", nlohmann::json::object(), harness.cancellation);
        auto const applied = harness.executor.executeSync(
          "set_program_snippet",
          {{"snippet",
            "// Function: main (ID: 3) [root]\n(vec3 color) main_3(vec3 pos)\n{\n"
            "    color = pos;\n}\n"}},
          harness.cancellation);
        ASSERT_TRUE(applied.value("success", false)) << applied.dump();
        (void)harness.executor.executeSync("commit_edit", nlohmann::json::object(), harness.cancellation);

        // Act
        auto const result = harness.executor.executeSync(
          "ensure_output_chain", {{"functionId", 3}}, harness.cancellation);

        // Assert: the failure comes before any resource is added, and it names the port
        EXPECT_FALSE(result.value("success", false)) << result.dump();
        EXPECT_NE(result.value("error", "").find("shape"), std::string::npos) << result.dump();
        EXPECT_EQ(collectRenderabilityFacts(harness.document.get()).levelSetCount, 0);
    }

    TEST(GetModelSummary_BlankDocument, ToolCall_ReportsWhyNothingRenders)
    {
        DocumentHarness harness;

        auto const result = harness.executor.executeSync("get_model_summary",
                                                         nlohmann::json::object(),
                                                         harness.cancellation);

        ASSERT_TRUE(result.value("success", false));
        EXPECT_EQ(result.value("buildItemCount", -1), 0);
        EXPECT_EQ(result.value("levelSetCount", -1), 0);
        EXPECT_FALSE(result.value("isRenderable", true));
        EXPECT_NE(result.value("notRenderedBecause", "").find("no_levelset"), std::string::npos)
          << result.dump();
    }

    TEST(CaptureViewport_BlankDocument, ToolCall_NotesTheEmptyScene)
    {
        DocumentHarness harness;

        auto const result = harness.executor.executeSync("capture_viewport",
                                                         {{"width", 64}, {"height", 64}},
                                                         harness.cancellation);

        // Assert: the image itself still comes back, but the reason for the blank scene rides along
        ASSERT_TRUE(result.value("success", false));
        EXPECT_NE(result.value("emptySceneNote", "").find("no_levelset"), std::string::npos)
          << result.dump();
    }
}
