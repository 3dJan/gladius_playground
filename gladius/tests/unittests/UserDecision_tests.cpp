#include "agent/UserDecision.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/TodoStore.h"
#include "io/CancellationToken.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/TransactionManager.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace gladius::agent::tests
{
    namespace
    {
        struct DecisionHarness
        {
            design::DesignGraph graph;
            design::DesignCatalogue catalogue;
            tools::TransactionManager transactions;
            tools::CheckpointManager checkpoints;
            design::TodoStore todos;
            KnowledgeStore knowledge;
            design::DesignToolContext ctx;
            tools::ToolRegistry registry;
            tools::ToolExecutor executor;
            io::CancellationToken cancellation;

            DecisionHarness()
                : knowledge(std::filesystem::temp_directory_path() / "gladius_user_decision_tests.json")
                , ctx{graph,
                      catalogue,
                      transactions,
                      checkpoints,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      &todos,
                      &knowledge}
                , executor(registry)
            {
                design::registerDesignTools(registry, ctx, design::DesignToolSurface::Requirements);
            }
            design::TodoItem const * findTodo(std::string const & id) const
            {
                for (auto const & item : todos.items())
                {
                    if (item.id == id)
                    {
                        return &item;
                    }
                }
                return nullptr;
            }
        };
    } // namespace

    TEST(TodoStore_CheckByUser, DeterministicEntry_MarksCheckedWithEvidenceAndAssumedFlag)
    {
        design::TodoStore store;
        std::string const id = store.addItem("Choose drivetrain",
                                             "Engine choice",
                                             {design::ChecklistEntry{.label = "User picked drivetrain",
                                                                     .kind = design::ChecklistKind::Geometry}});

        std::string reason;
        ASSERT_TRUE(store.checkByUser(
          id, 0, nlohmann::json{{"choice", "Electric"}}, true, &reason))
          << reason;

        design::TodoItem const * item = nullptr;
        for (auto const & candidate : store.items())
        {
            if (candidate.id == id)
            {
                item = &candidate;
                break;
            }
        }
        ASSERT_NE(item, nullptr);
        ASSERT_EQ(item->checklist.size(), 1u);
        EXPECT_TRUE(item->checklist[0].checked);
        EXPECT_TRUE(item->checklist[0].assumed);
        EXPECT_EQ(item->checklist[0].evidence.value("choice", ""), "Electric");

        auto const serialized = store.toJson();
        ASSERT_TRUE(serialized.contains("items"));
        ASSERT_EQ(serialized["items"].size(), 1u);
        EXPECT_TRUE(serialized["items"][0]["checklist"][0].value("assumed", false));
    }

    TEST(TodoStore_CheckByUser, UnknownIdOrBadIndex_RefusesWithReason)
    {
        design::TodoStore store;
        std::string const id = store.addItem(
          "Item", "Desc", {design::ChecklistEntry{.label = "Step 1"}});

        std::string reason;
        EXPECT_FALSE(store.checkByUser("missing_id", 0, nlohmann::json{{"choice", "A"}}, false, &reason));
        EXPECT_FALSE(reason.empty());

        reason.clear();
        EXPECT_FALSE(store.checkByUser(id, 4, nlohmann::json{{"choice", "A"}}, false, &reason));
        EXPECT_FALSE(reason.empty());
    }

    TEST(AskUserTool_WithoutCallback, ToolCall_RefusesWithoutHanging)
    {
        DecisionHarness harness;

        auto const result = harness.executor.executeSync(
          "ask_user", {{"question", "Combustion or electric?"}}, harness.cancellation);

        EXPECT_FALSE(result.value("success", true));
        EXPECT_FALSE(result.value("error", std::string{}).empty());
    }

    TEST(AskUserTool_BudgetReached, ToolCall_RefusesAfterLimit)
    {
        DecisionHarness harness;
        UserDecisionHandoff handoff;
        harness.ctx.askUser = makeAskUserCallback(handoff, DecisionPolicy::UseRecommendation);
        harness.ctx.userQuestionsAsked = kMaxUserQuestions;

        auto const result = harness.executor.executeSync(
          "ask_user",
          {{"question", "Pick one"}, {"options", {"A", "B"}}, {"recommended", 0}},
          harness.cancellation);

        EXPECT_FALSE(result.value("success", true));
        EXPECT_NE(result.value("error", std::string{}).find("budget"), std::string::npos);
    }

    TEST(AskUserTool_UseRecommendation, ToolCall_SelectsRecommendedVariantAndMarksAssumed)
    {
        DecisionHarness harness;
        UserDecisionHandoff handoff;
        harness.ctx.askUser = makeAskUserCallback(handoff, DecisionPolicy::UseRecommendation);

        auto const nodeId = harness.graph.addNode(design::DesignNodeType::Component, "Engine");
        auto const vComb = harness.graph.createVariant(nodeId, "Combustion", "V6", design::FidelityLevel::Envelope);
        auto const vElec = harness.graph.createVariant(nodeId, "Electric", "Hub motor", design::FidelityLevel::Envelope);
        auto const todoId = harness.todos.addItem("Pick engine",
                                                  "Drivetrain",
                                                  {design::ChecklistEntry{.label = "Engine chosen",
                                                                          .kind = design::ChecklistKind::User}});

        auto const result = harness.executor.executeSync(
          "ask_user",
          {{"question", "Which engine?"},
           {"options", {"Combustion", "Electric"}},
           {"optionVariantIds", {vComb, vElec}},
           {"recommended", 1},
           {"nodeId", nodeId},
           {"todoId", todoId},
           {"checkIndex", 0}},
          harness.cancellation);

        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_EQ(result.value("choice", ""), "Electric");
        EXPECT_TRUE(result.value("assumed", false));
        EXPECT_EQ(result.value("appliedVariantId", ""), vElec);
        EXPECT_TRUE(result.value("checklistRecorded", false));

        auto const active = harness.graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->selectedVariants.at(nodeId), vElec);

        auto const * todo = harness.findTodo(todoId);
        ASSERT_NE(todo, nullptr);
        EXPECT_TRUE(todo->checklist[0].checked);
        EXPECT_TRUE(todo->checklist[0].assumed);
    }

    TEST(AskUserTool_InteractiveHandoff, FreeTextMatchingOption_SelectsOptionAndAppliesVariant)
    {
        DecisionHarness harness;
        UserDecisionHandoff handoff;
        harness.ctx.askUser = makeAskUserCallback(handoff, DecisionPolicy::AskUser);

        auto const nodeId = harness.graph.addNode(design::DesignNodeType::Component, "Engine");
        auto const vComb = harness.graph.createVariant(nodeId, "Combustion", "V6", design::FidelityLevel::Envelope);
        auto const vElec = harness.graph.createVariant(nodeId, "Electric", "Hub motor", design::FidelityLevel::Envelope);

        std::thread responder([&]() {
            while (!handoff.isAwaiting())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            handoff.answerText("combustion");
        });

        auto const result = harness.executor.executeSync(
          "ask_user",
          {{"question", "Which engine?"},
           {"options", {"Combustion", "Electric"}},
           {"optionVariantIds", {vComb, vElec}},
           {"nodeId", nodeId}},
          harness.cancellation);

        responder.join();

        ASSERT_TRUE(result.value("success", false)) << result.dump();
        EXPECT_EQ(result.value("choice", ""), "Combustion");
        EXPECT_FALSE(result.value("freeText", true));
        EXPECT_FALSE(result.value("assumed", true));
        EXPECT_EQ(result.value("appliedVariantId", ""), vComb);
    }

    TEST(AskUserTool_InteractiveHandoff, Abandon_CancelsAwaitingQuestion)
    {
        DecisionHarness harness;
        UserDecisionHandoff handoff;
        harness.ctx.askUser = makeAskUserCallback(handoff, DecisionPolicy::AskUser);

        std::thread stopper([&]() {
            while (!handoff.isAwaiting())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            handoff.abandon("Run stopped");
        });

        auto const result = harness.executor.executeSync(
          "ask_user",
          {{"question", "Which material?"}, {"options", {"PLA", "PETG"}}},
          harness.cancellation);

        stopper.join();

        EXPECT_FALSE(result.value("success", true));
        EXPECT_EQ(result.value("error", ""), "Run stopped");
    }

    TEST(ScriptedAskUserCallback_IndexSubstringAndFallback, ResolvesDeterministically)
    {
        std::vector<ScriptedDecision> log;
        auto cb = makeScriptedAskUserCallback({"1", "elec"}, log, /*fallbackToRecommendation=*/true);
        io::CancellationToken cancellation;

        UserQuestion q1{.id = "q_1", .question = "Material?", .options = {"PLA", "PETG"}, .recommended = 0};
        UserQuestion q2{.id = "q_2", .question = "Motor?", .options = {"Combustion", "Electric"}, .recommended = 0};
        UserQuestion q3{.id = "q_3", .question = "Color?", .options = {"Red", "Blue"}, .recommended = 1};

        auto const r1 = coro::sync_wait(cb(q1, cancellation));
        auto const r2 = coro::sync_wait(cb(q2, cancellation));
        auto const r3 = coro::sync_wait(cb(q3, cancellation));

        EXPECT_EQ(r1.answer.choice, "PETG");
        EXPECT_EQ(r2.answer.choice, "Electric");
        EXPECT_EQ(r3.answer.choice, "Blue");
        EXPECT_TRUE(r3.answer.assumed);
        ASSERT_EQ(log.size(), 3u);
        EXPECT_EQ(log[0].script, "1");
        EXPECT_EQ(log[1].script, "elec");
        EXPECT_TRUE(log[2].script.empty());
    }

    TEST(ScriptedAskUserCallback_AmbiguousSubstring, Callback_Refuses)
    {
        std::vector<ScriptedDecision> log;
        auto cb = makeScriptedAskUserCallback({"motor"}, log, /*fallbackToRecommendation=*/false);
        io::CancellationToken cancellation;

        UserQuestion q{.id = "q_1",
                       .question = "Motor?",
                       .options = {"Stepper motor", "Servo motor"},
                       .recommended = 0};

        auto const r = coro::sync_wait(cb(q, cancellation));

        EXPECT_FALSE(r.error.empty());
        ASSERT_EQ(log.size(), 1u);
        EXPECT_TRUE(log[0].refused);
    }
} // namespace gladius::agent::tests
