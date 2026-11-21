#include "agent/providers/AnthropicProvider.h"
#include "agent/providers/OpenAiProvider.h"

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace gladius::agent::tests
{
    namespace
    {
        class MockHttpTransport final : public IHttpTransport
        {
          public:
            std::vector<std::pair<std::string, std::string>> sseEvents;
            HttpResponse nextResponse{200, "{}", "", false};
            HttpRequest lastRequest;

            HttpResponse postJson(HttpRequest const & request,
                                  io::CancellationToken const & /*cancellation*/) override
            {
                lastRequest = request;
                return nextResponse;
            }

            HttpResponse postSse(HttpRequest const & request,
                                 SseEventCallback onEvent,
                                 io::CancellationToken const & /*cancellation*/) override
            {
                lastRequest = request;
                for (auto const & [ev, data] : sseEvents)
                {
                    onEvent(ev, data);
                }
                return nextResponse;
            }
        };
    } // namespace

    TEST(SseStreamParser, Feed_ChunkedEvents_ParsesEventAndData)
    {
        // Arrange
        std::vector<std::pair<std::string, std::string>> received;
        SseStreamParser parser([&](std::string const & ev, std::string const & data) {
            received.emplace_back(ev, data);
        });

        // Act
        parser.feed("event: content_block_delta\ndata: {\"a\":");
        parser.feed("1}\n\n: keepalive\n\ndata: [DONE]\n\n");
        parser.finish();

        // Assert
        ASSERT_EQ(received.size(), 2u);
        EXPECT_EQ(received[0].first, "content_block_delta");
        EXPECT_EQ(received[0].second, "{\"a\":1}");
        EXPECT_EQ(received[1].first, "");
        EXPECT_EQ(received[1].second, "[DONE]");
    }

    TEST(OpenAiProvider, Complete_StreamingToolCallAndText_AssemblesResponse)
    {
        // Arrange
        auto mock = std::make_shared<MockHttpTransport>();
        mock->sseEvents = {
            {"", R"({"choices":[{"delta":{"content":"Hello "}}]})"},
            {"", R"({"choices":[{"delta":{"content":"world"}}]})"},
            {"", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"get_bbox","arguments":"{\"id\":"}}]}}]})"},
            {"", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"7}"}}]},"finish_reason":"tool_calls"}]})"},
            {"", "[DONE]"}
        };

        ProviderConfig cfg;
        cfg.model = "gpt-test";
        cfg.apiKey = "sk-test";
        cfg.stream = true;
        OpenAiProvider provider(cfg, mock);

        CompletionRequest req;
        req.systemPrompt = "You are a designer.";
        req.messages.push_back(Message{MessageRole::User, {TextContent{"Inspect"}}});

        std::string streamed;
        io::CancellationToken cancel;

        // Act
        auto const res = coro::sync_wait(
          provider.complete(req, [&](std::string const & d) { streamed += d; }, cancel));

        // Assert
        EXPECT_TRUE(res.success);
        EXPECT_EQ(res.text, "Hello world");
        EXPECT_EQ(streamed, "Hello world");
        EXPECT_EQ(res.stopReason, "tool_calls");
        ASSERT_EQ(res.toolCalls.size(), 1u);
        EXPECT_EQ(res.toolCalls[0].id, "call_1");
        EXPECT_EQ(res.toolCalls[0].name, "get_bbox");
        EXPECT_EQ(res.toolCalls[0].arguments.value("id", 0), 7);
    }

    TEST(AnthropicProvider, Complete_StreamingTextAndToolUse_AssemblesResponse)
    {
        // Arrange
        auto mock = std::make_shared<MockHttpTransport>();
        mock->sseEvents = {
            {"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
            {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Thinking..."}})"},
            {"content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"validate_model"}})"},
            {"content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"strict\":true}"}})"},
            {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})"}
        };

        ProviderConfig cfg;
        cfg.model = "claude-test";
        cfg.apiKey = "ant-test";
        cfg.stream = true;
        AnthropicProvider provider(cfg, mock);

        CompletionRequest req;
        req.systemPrompt = "System prompt";
        req.messages.push_back(Message{MessageRole::User, {TextContent{"Validate"}}});

        std::string streamed;
        io::CancellationToken cancel;

        // Act
        auto const res = coro::sync_wait(
          provider.complete(req, [&](std::string const & d) { streamed += d; }, cancel));

        // Assert
        EXPECT_TRUE(res.success);
        EXPECT_EQ(res.text, "Thinking...");
        EXPECT_EQ(streamed, "Thinking...");
        EXPECT_EQ(res.stopReason, "tool_use");
        ASSERT_EQ(res.toolCalls.size(), 1u);
        EXPECT_EQ(res.toolCalls[0].id, "toolu_1");
        EXPECT_EQ(res.toolCalls[0].name, "validate_model");
        EXPECT_TRUE(res.toolCalls[0].arguments.value("strict", false));
    }
} // namespace gladius::agent::tests
