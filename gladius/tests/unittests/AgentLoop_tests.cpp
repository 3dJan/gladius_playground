#include "agent/AgentLoop.h"

#include <gtest/gtest.h>
#include <queue>

namespace gladius::agent::tests
{
    namespace
    {
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
} // namespace gladius::agent::tests
