#include "agent/AgentLoop.h"
#include "agent/KnowledgeStore.h"
#include "agent/SkillLoader.h"
#include "agent/providers/FallbackProvider.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <chrono>
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <variant>

namespace gladius::agent::tests
{
    namespace
    {
        std::string messageText(Message const & message)
        {
            std::string text;
            for (auto const & part : message.parts)
            {
                if (auto const * content = std::get_if<TextContent>(&part))
                {
                    text += content->text;
                }
            }
            return text;
        }

        class ScriptedProvider final : public IAiProvider
        {
          public:
            std::queue<CompletionResponse> responses;
            std::vector<CompletionRequest> capturedRequests;

            std::string name() const override
            {
                return "scripted";
            }

            coro::task<CompletionResponse> complete(CompletionRequest const & request,
                                                    TextDeltaCallback onDelta,
                                                    io::CancellationToken const & /*cancellation*/) override
            {
                capturedRequests.push_back(request);
                if (responses.empty())
                {
                    co_return CompletionResponse{"Done", {}, "stop", true, ""};
                }
                auto resp = std::move(responses.front());
                responses.pop();
                if (onDelta && !resp.text.empty())
                {
                    onDelta(resp.text);
                }
                co_return resp;
            }
        };
    } // namespace

    TEST(AgentLoop, Run_MultiTurnToolCall_ExecutesAndCompletes)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
            "Finishing now",
            {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
            "tool_calls",
            true,
            ""});

        Conversation conv;
        conv.appendUserText("Design a lamp");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_EQ(res.modelTurns, 1u);
        EXPECT_EQ(res.toolCalls, 1u);
        EXPECT_TRUE(finished);
        auto const events = queue.drain();
        EXPECT_FALSE(events.empty());
    }

    TEST(AgentLoop, Run_DesignAlreadyComplete_KeepsIteratingFollowUpRequest)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"get_model_summary", "Inspect", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
          "Looking at the scene.",
          {ToolCall{"c1", "get_model_summary", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});
        provider.responses.push(CompletionResponse{"I will extend the design.", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Build a house out of the brick I added");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act: the graph is already Complete when the follow-up prompt arrives
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          []() { return true; }));

        // Assert: the stale complete state must not cut the run short after the first tool call
        EXPECT_TRUE(res.completed);
        EXPECT_EQ(res.modelTurns, 2u);
        EXPECT_EQ(res.toolCalls, 1u);
    }

    TEST(AgentLoop, Build_FollowUpRequestInjectsReIterationGuidance)
    {
        // Arrange
        Conversation conv;
        conv.appendUserText("Build a lego house");
        tools::ToolRegistry registry;
        ContextBuilder ctxBuilder;
        DesignContextSnapshot snap;
        snap.isFollowUpRequest = true;
        snap.requirementsSummary = "- req_1 [functional, mandatory] \"Fit a 2x4 brick\"\n";

        // Act
        auto const req = ctxBuilder.build(conv, registry, PrivacySettings{}, snap);

        // Assert: run-volatile guidance travels in the last user message, never in the system
        // prompt, because changing the system block invalidates the provider prompt cache.
        EXPECT_NE(messageText(req.messages.back()).find("FOLLOW-UP REQUEST ON A FINISHED DESIGN"),
                  std::string::npos);
        EXPECT_NE(messageText(req.messages.back()).find("Requirements:"), std::string::npos);
        EXPECT_EQ(req.systemPrompt.find("FOLLOW-UP REQUEST ON A FINISHED DESIGN"), std::string::npos);

        DesignContextSnapshot const plain;
        auto const plainReq = ctxBuilder.build(conv, registry, PrivacySettings{}, plain);
        EXPECT_EQ(messageText(plainReq.messages.back()).find("FOLLOW-UP REQUEST ON A FINISHED DESIGN"),
                  std::string::npos);
    }

    TEST(AgentLoop, Run_PrivacyBlocksViewport_DeniesToolCall)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool captured = false;
        registry.registerSyncTool(
          {"capture_viewport", "Capture", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [&](nlohmann::json const &) {
              captured = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
            "",
            {ToolCall{"c1", "capture_viewport", nlohmann::json::object()}},
            "tool_calls",
            true,
            ""});
        provider.responses.push(CompletionResponse{"Okay, skipped viewport.", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Check viewport");
        ContextBuilder ctxBuilder;
        PrivacySettings privacy;
        privacy.allowViewportImages = false;
        PermissionPolicy policy(AutonomyLevel::WorkAutonomously, privacy);
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_FALSE(captured);
    }

    TEST(AgentLoop, Run_MaxModelTurnsReached_FailsCleanly)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"ping", "Ping", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"Turn 1", {ToolCall{"c1", "ping", {}}}, "tool_calls", true, ""});
        provider.responses.push(
          CompletionResponse{"Turn 2", {ToolCall{"c2", "ping", {}}}, "tool_calls", true, ""});

        Conversation conv;
        conv.appendUserText("Loop");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxModelTurns = 2;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act (tool calls keep the loop running until maxModelTurns)
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          []() { return false; }));

        // Assert
        EXPECT_FALSE(res.completed);
        EXPECT_EQ(res.modelTurns, 2u);
        EXPECT_EQ(res.error, "Maximum model turn limit reached");
    }

    namespace
    {
        class SlowProvider final : public IAiProvider
        {
          public:
            /// @brief When non-zero, emit finish_design instead of ping once this many turns were served
            std::size_t turnsBeforeFinish = 0;

            std::string name() const override
            {
                return "slow";
            }

            coro::task<CompletionResponse> complete(CompletionRequest const &,
                                                    TextDeltaCallback,
                                                    io::CancellationToken const &) override
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ++turns;
                if (turnsBeforeFinish != 0 && turns >= turnsBeforeFinish)
                {
                    co_return CompletionResponse{"All done",
                                                 {ToolCall{"cf", "finish_design", {}}},
                                                 "tool_calls",
                                                 true,
                                                 ""};
                }
                co_return CompletionResponse{"Working",
                                             {ToolCall{"c1", "ping", {}}},
                                             "tool_calls",
                                             true,
                                             ""};
            }

          private:
            std::size_t turns = 0;
        };
    } // namespace

    TEST(AgentLoop, Run_WallTimeLimitExceeded_FailsCleanly)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"ping", "Ping", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        tools::ToolExecutor executor(registry);
        SlowProvider provider;
        Conversation conv;
        conv.appendUserText("Wait forever");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxWallTime = std::chrono::milliseconds(1);
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act (tool calls keep the loop running past the wall-time budget)
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          []() { return false; }));

        // Assert
        EXPECT_FALSE(res.completed);
        EXPECT_EQ(res.error, "Agent wall-time limit reached");
    }

    TEST(AgentLoop, Run_ZeroWallTime_UnlimitedRunsToCompletion)
    {
        // Arrange (a 0 duration used to mean "already expired")
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"ping", "Ping", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);
        SlowProvider provider;
        provider.turnsBeforeFinish = 3;
        Conversation conv;
        conv.appendUserText("No time limit");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxWallTime = std::chrono::milliseconds(0);
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_TRUE(res.error.empty());
    }

    TEST(AgentLoop, Run_ZeroMaxModelTurns_UnlimitedRunsToCompletion)
    {
        // Arrange (a 0 turn limit used to stop the loop immediately)
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"ping", "Ping", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);
        SlowProvider provider;
        provider.turnsBeforeFinish = 70;
        Conversation conv;
        conv.appendUserText("No turn limit");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxModelTurns = 0;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act (more turns than the default 64 turn limit)
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_TRUE(res.error.empty());
    }

    TEST(AgentLimits, Defaults_AreUnlimited)
    {
        // Arrange & Act
        AgentLimits const limits;

        // Assert (long design runs must not be cut short out of the box)
        EXPECT_EQ(limits.maxModelTurns, 0);
        EXPECT_EQ(limits.maxToolCalls, 0u);
        EXPECT_EQ(limits.maxViewportReviews, 0u);
        EXPECT_EQ(limits.maxWallTime, std::chrono::milliseconds::zero());
    }

    namespace
    {
        /// @brief Waits for the loop to park on a limit and answers it from another thread
        void resumeWhenParked(AgentRunControl & runControl, bool unlimitedFromNow)
        {
            while (!runControl.isPaused())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            runControl.resume(unlimitedFromNow);
        }

        /// @brief First limit event in the queue, if any
        std::optional<AgentLimitReachedEvent> limitEvent(AgentEventQueue & queue)
        {
            for (auto const & event : queue.drain())
            {
                if (auto const * hit = std::get_if<AgentLimitReachedEvent>(&event))
                {
                    return *hit;
                }
            }
            return std::nullopt;
        }

        void registerPingAndFinish(tools::ToolRegistry & registry, bool & finished)
        {
            registry.registerSyncTool(
              {"ping", "Ping", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
              [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
            registry.registerSyncTool(
              {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
              [&](nlohmann::json const &) {
                  finished = true;
                  return nlohmann::json{{"success", true}};
              });
        }
    } // namespace

    TEST(AgentLoop, Run_ModelTurnLimitWithRunControl_ParksAndContinues)
    {
        // Arrange (a parked limit is a user decision, not a failed provider call)
        tools::ToolRegistry registry;
        bool finished = false;
        registerPingAndFinish(registry, finished);
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"Turn 1", {ToolCall{"c1", "ping", {}}}, "tool_calls", true, ""});
        provider.responses.push(
          CompletionResponse{"Turn 2", {ToolCall{"c2", "ping", {}}}, "tool_calls", true, ""});
        provider.responses.push(CompletionResponse{"Done", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Loop");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxModelTurns = 2;
        io::CancellationToken cancel;
        AgentRunControl runControl;
        AgentLoop loop;

        // Act (helper thread continues the parked run with a fresh segment budget)
        std::thread resumper([&runControl]() { resumeWhenParked(runControl, false); });
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }, nullptr, nullptr, nullptr, &runControl));
        resumper.join();

        // Assert
        auto const hit = limitEvent(queue);
        ASSERT_TRUE(hit.has_value());
        EXPECT_EQ(hit->kind, AgentLimitKind::ModelTurns);
        EXPECT_EQ(hit->used, 2u);
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(res.error.empty());
        EXPECT_GT(res.modelTurns, static_cast<std::size_t>(limits.maxModelTurns));
    }

    TEST(AgentLoop, Run_ContinueUnlimited_LiftsRemainingLimits)
    {
        // Arrange (needs far more turns than the segment budget to prove the caps are gone)
        tools::ToolRegistry registry;
        bool finished = false;
        registerPingAndFinish(registry, finished);
        tools::ToolExecutor executor(registry);
        SlowProvider provider;
        provider.turnsBeforeFinish = 70;
        Conversation conv;
        conv.appendUserText("Keep going");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxModelTurns = 3;
        io::CancellationToken cancel;
        AgentRunControl runControl;
        AgentLoop loop;

        // Act
        std::thread resumper([&runControl]() { resumeWhenParked(runControl, true); });
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }, nullptr, nullptr, nullptr, &runControl));
        resumper.join();

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_TRUE(res.error.empty());
        EXPECT_GE(res.modelTurns, 70u);
    }

    TEST(AgentLoop, Run_CancelledWhileParkedOnLimit_ReportsCancellation)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool finished = false;
        registerPingAndFinish(registry, finished);
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"Turn 1", {ToolCall{"c1", "ping", {}}}, "tool_calls", true, ""});
        provider.responses.push(
          CompletionResponse{"Turn 2", {ToolCall{"c2", "ping", {}}}, "tool_calls", true, ""});

        Conversation conv;
        conv.appendUserText("Loop");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        limits.maxModelTurns = 2;
        io::CancellationToken cancel;
        AgentRunControl runControl;
        AgentLoop loop;

        // Act (the user stops the run while it waits for a continue decision)
        std::thread canceller([&runControl, &cancel]() {
            while (!runControl.isPaused())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            cancel.requestCancellation();
            runControl.resume(false);
        });
        auto const res = coro::sync_wait(loop.run(
          conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel,
          [&]() { return finished; }, nullptr, nullptr, nullptr, &runControl));
        canceller.join();

        // Assert
        EXPECT_TRUE(res.cancelled);
        EXPECT_FALSE(res.completed);
        EXPECT_EQ(res.error, "Agent run cancelled");
    }

    TEST(AgentLoop, Run_CaptureViewportTool_AttachesImageContentAndStripsBase64)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"capture_viewport", "Capture", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &) {
              return nlohmann::json{
                {"success", true},
                {"mimeType", "image/png"},
                {"base64", "iVBORw0KGgo="}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
          "",
          {ToolCall{"c1", "capture_viewport", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});
        provider.responses.push(CompletionResponse{"Looks good.", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Check viewport");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy(AutonomyLevel::WorkAutonomously, PrivacySettings{});
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_TRUE(res.completed);
        ASSERT_GE(provider.capturedRequests.size(), 2u);
        auto const & secondMsgs = provider.capturedRequests[1].messages;
        bool sawImage = false;
        bool sawBase64Text = false;
        for (auto const & m : secondMsgs)
        {
            for (auto const & p : m.parts)
            {
                if (std::holds_alternative<ImageContent>(p))
                {
                    sawImage = true;
                }
                if (auto const * tr = std::get_if<ToolResult>(&p))
                {
                    if (tr->value.contains("base64"))
                    {
                        sawBase64Text = true;
                    }
                }
            }
        }
        EXPECT_TRUE(sawImage);
        EXPECT_FALSE(sawBase64Text);
    }

    TEST(FallbackProvider, Complete_WhenPrimaryFails_FallsBackToSecondary)
    {
        // Arrange
        auto primary = std::make_unique<ScriptedProvider>();
        primary->responses.push(CompletionResponse{{}, {}, "", false, "HTTP 429 rate limit"});
        auto secondary = std::make_unique<ScriptedProvider>();
        secondary->responses.push(CompletionResponse{"Recovered via fallback", {}, "stop", true, ""});

        std::vector<FallbackProvider::Candidate> candidates;
        candidates.push_back(FallbackProvider::Candidate{"primary", std::move(primary)});
        candidates.push_back(FallbackProvider::Candidate{"secondary", std::move(secondary)});
        FallbackProvider fallback(std::move(candidates));

        CompletionRequest req;
        io::CancellationToken cancel;

        // Act
        auto const resp = coro::sync_wait(fallback.complete(req, nullptr, cancel));

        // Assert
        EXPECT_TRUE(resp.success);
        EXPECT_EQ(resp.text, "Recovered via fallback");
    }

    TEST(KnowledgeStore, StoreSearchUpdateDelete_PersistsAndRanksResults)
    {
        // Arrange
        std::filesystem::path const tmpPath =
          std::filesystem::temp_directory_path() / "gladius_test_knowledge.json";
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);

        KnowledgeStore store(tmpPath);
        auto const k1 = store.store(
          "Gyroid wall thickness",
          "lattices",
          {"gyroid", "fdm", "wall"},
          "Keep gyroid minimum wall >= 1.2mm for 0.4mm nozzle FDM prints.");
        (void) store.store(
          "Gear tooth clearance",
          "mechanical",
          {"gear", "clearance"},
          "Use 0.25mm radial backlash clearance for printed spur gears.");

        // Act
        auto const hits = store.search("gyroid fdm", "", 5);
        ASSERT_FALSE(hits.empty());
        EXPECT_EQ(hits[0].id, k1);

        ASSERT_TRUE(store.update(k1, "", "", {}, "Keep gyroid wall >= 1.4mm for strength."));

        KnowledgeStore reloaded(tmpPath);
        ASSERT_TRUE(reloaded.load());
        auto const reloadedHits = reloaded.search("gyroid", "", 5);

        // Assert
        ASSERT_EQ(reloadedHits.size(), 1u);
        EXPECT_EQ(reloadedHits[0].summary, "Keep gyroid wall >= 1.4mm for strength.");
        EXPECT_TRUE(reloaded.remove(k1));
        std::filesystem::remove(tmpPath, ec);
    }

    TEST(AgentLoop, Run_AutonomousModeHesitatesWithoutTools_NudgesAndCompletes)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{"Should I continue?", {}, "stop", true, ""});
        provider.responses.push(CompletionResponse{
          "Continuing autonomously.",
          {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        Conversation conv;
        conv.appendUserText("Design a bracket");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy(AutonomyLevel::WorkAutonomously, PrivacySettings{});
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(loop.run(
          conv,
          provider,
          registry,
          executor,
          ctxBuilder,
          policy,
          queue,
          limits,
          cancel,
          [&]() { return finished; },
          nullptr,
          []() {
              DesignContextSnapshot snap;
              snap.autonomy = AutonomyLevel::WorkAutonomously;
              return snap;
          }));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_EQ(res.modelTurns, 2u);
        EXPECT_NE(provider.capturedRequests[0].systemPrompt.find("AUTONOMOUS MODE ACTIVE"),
                  std::string::npos);
    }

    TEST(AgentLoop, Run_PausedBeforeModelTurn_WaitsThenResumes)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
          "Finishing now",
          {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        Conversation conv;
        conv.appendUserText("Design a lamp");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentRunControl runControl;
        AgentLoop loop;
        runControl.requestPause();

        // Act: the run is started while paused, so it must not reach the model yet.
        AgentRunResult res;
        std::thread runner(
          [&]()
          {
              res = coro::sync_wait(loop.run(
                conv,
                provider,
                registry,
                executor,
                ctxBuilder,
                policy,
                queue,
                limits,
                cancel,
                [&]() { return finished; },
                nullptr,
                nullptr,
                nullptr,
                &runControl));
          });

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        EXPECT_TRUE(provider.capturedRequests.empty());

        runControl.resume();
        runner.join();

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_FALSE(provider.capturedRequests.empty());
    }

    TEST(AgentLoop, Run_TextOnlyQuestion_AnnouncesAwaitingUserReply)
    {
        // Arrange
        tools::ToolRegistry registry;
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"Should I use a hollow shell or a solid core?", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Design a housing");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert: the question is the last thing the user must see, not a collapsed log line
        EXPECT_TRUE(res.completed);
        auto events = queue.drain();
        auto const * completed = std::get_if<AgentCompletedEvent>(&events.back());
        ASSERT_NE(completed, nullptr);
        EXPECT_TRUE(completed->awaitingUserReply);
    }

    TEST(AgentLoop, Run_TextOnlyStatement_ClosesRunWithoutWaiting)
    {
        // Arrange
        tools::ToolRegistry registry;
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"The bracket is validated and finished.", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Finish the bracket");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_TRUE(res.completed);
        auto events = queue.drain();
        auto const * completed = std::get_if<AgentCompletedEvent>(&events.back());
        ASSERT_NE(completed, nullptr);
        EXPECT_FALSE(completed->awaitingUserReply);
    }

    TEST(AgentLoop, Run_QuestionWhileAutonomous_KeepsNudgingInsteadOfWaiting)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(
          CompletionResponse{"Should I use a hollow shell or a solid core?", {}, "stop", true, ""});

        Conversation conv;
        conv.appendUserText("Design a housing");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy(AutonomyLevel::WorkAutonomously, PrivacySettings{});
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert: the nudge keeps the run going instead of ending on a question
        EXPECT_GT(res.modelTurns, 1u);
        for (auto const & ev : queue.drain())
        {
            if (auto const * completed = std::get_if<AgentCompletedEvent>(&ev))
            {
                EXPECT_FALSE(completed->awaitingUserReply);
            }
        }
    }

    TEST(AgentLoop, Run_EmptyCompletionThenToolCall_RetriesInsteadOfCompleting)
    {
        // Arrange
        tools::ToolRegistry registry;
        bool finished = false;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [&](nlohmann::json const &) {
              finished = true;
              return nlohmann::json{{"success", true}};
          });
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{"", {}, "max_tokens", true, ""});
        provider.responses.push(CompletionResponse{
          "Finishing now",
          {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        Conversation conv;
        conv.appendUserText("Design a bracket");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_TRUE(res.completed);
        EXPECT_TRUE(finished);
        EXPECT_EQ(res.emptyTurns, 1u);
        bool correctiveSent = false;
        for (auto const & message : provider.capturedRequests[1].messages)
        {
            correctiveSent = correctiveSent || messageText(message).find("reply was empty")
                             != std::string::npos;
        }
        EXPECT_TRUE(correctiveSent);
        auto const events = queue.drain();
        EXPECT_TRUE(std::get_if<AgentFailedEvent>(&events.back()) == nullptr);
    }

    TEST(AgentLoop, Run_EmptyCompletionsAtOutputLimit_FailsWithActionableError)
    {
        // Arrange
        tools::ToolRegistry registry;
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        for (int i = 0; i < 3; ++i)
        {
            provider.responses.push(CompletionResponse{"", {}, "max_tokens", true, ""});
        }

        Conversation conv;
        conv.appendUserText("Design a housing");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_FALSE(res.completed);
        EXPECT_NE(res.error.find("output limit"), std::string::npos);
        EXPECT_EQ(res.emptyTurns, 3u);
        auto events = queue.drain();
        auto const * failedEvent = std::get_if<AgentFailedEvent>(&events.back());
        ASSERT_NE(failedEvent, nullptr);
        EXPECT_EQ(failedEvent->errorInfo.kind, ProviderErrorKind::MalformedResponse);
        EXPECT_NE(failedEvent->errorInfo.message.find("output limit"), std::string::npos);
        EXPECT_NE(failedEvent->errorInfo.hint.find("max output tokens"), std::string::npos);
    }

    TEST(AgentLoop, Run_EmptyCompletionWithoutTruncation_FailsWithUnknownStopReason)
    {
        // Arrange
        tools::ToolRegistry registry;
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        for (int i = 0; i < 3; ++i)
        {
            provider.responses.push(CompletionResponse{"", {}, "", true, ""});
        }

        Conversation conv;
        conv.appendUserText("Design a housing");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_FALSE(res.completed);
        EXPECT_EQ(res.error, "Model returned an empty answer");
        auto events = queue.drain();
        auto const * failedEvent = std::get_if<AgentFailedEvent>(&events.back());
        ASSERT_NE(failedEvent, nullptr);
        EXPECT_NE(failedEvent->errorInfo.message.find("answered with nothing"), std::string::npos);
    }

    TEST(AgentLoop, Run_ProviderFails_ClassifiesErrorForThePanel)
    {
        // Arrange
        tools::ToolRegistry registry;
        tools::ToolExecutor executor(registry);

        ScriptedProvider provider;
        CompletionResponse failed{"", {}, "", false, "HTTP 401: Invalid API key"};
        provider.responses.push(failed);

        Conversation conv;
        conv.appendUserText("Design a housing");
        ContextBuilder ctxBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancel;
        AgentLoop loop;

        // Act
        auto const res = coro::sync_wait(
          loop.run(conv, provider, registry, executor, ctxBuilder, policy, queue, limits, cancel));

        // Assert
        EXPECT_FALSE(res.completed);
        auto events = queue.drain();
        auto const * failedEvent = std::get_if<AgentFailedEvent>(&events.back());
        ASSERT_NE(failedEvent, nullptr);
        EXPECT_EQ(failedEvent->errorInfo.kind, ProviderErrorKind::InvalidApiKey);
    }

    TEST(SkillLoader, BuiltInSkills_ContainsModelingAndLibraryReuse)
    {
        SkillLoader loader;
        EXPECT_TRUE(loader.findSkill("sdf-modeling").has_value());
        EXPECT_TRUE(loader.findSkill("library-reuse").has_value());
        EXPECT_TRUE(loader.findSkill("morphological-design").has_value());
        EXPECT_FALSE(loader.summarizeForPrompt().empty());
    }
} // namespace gladius::agent::tests
