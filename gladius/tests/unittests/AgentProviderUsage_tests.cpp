#include "agent/providers/AnthropicProvider.h"
#include "agent/providers/OpenAiProvider.h"
#include "agent/providers/FallbackProvider.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace gladius::agent::tests
{
    namespace
    {
        /// @brief Returns canned JSON or SSE frames and records the request body
        class MockTransport final : public IHttpTransport
        {
          public:
            MockTransport(std::string jsonBody, std::vector<std::pair<std::string, std::string>> sse)
                : m_jsonBody(std::move(jsonBody))
                , m_sseFrames(std::move(sse))
            {
            }

            /// @brief Delay before the first SSE frame, so streamed timing is measurable
            MockTransport & withDelay(std::chrono::milliseconds delay) noexcept
            {
                m_delay = delay;
                return *this;
            }

            [[nodiscard]] HttpResponse postJson(HttpRequest const & request,
                                                io::CancellationToken const &) override
            {
                capturedBody = request.body;
                if (m_delay.count() > 0)
                {
                    std::this_thread::sleep_for(m_delay);
                }
                return HttpResponse{200, m_jsonBody, "", false};
            }

            [[nodiscard]] HttpResponse postSse(HttpRequest const & request,
                                               SseEventCallback onEvent,
                                               io::CancellationToken const &) override
            {
                capturedBody = request.body;
                for (auto const & [event, data] : m_sseFrames)
                {
                    onEvent(event, data);
                }
                return HttpResponse{200, "", "", false};
            }

            [[nodiscard]] coro::task<HttpResponse>
            postSseAsync(HttpRequest const & request,
                         SseEventCallback onEvent,
                         io::CancellationToken const &) override
            {
                capturedBody = request.body;
                if (m_delay.count() > 0)
                {
                    std::this_thread::sleep_for(m_delay);
                }
                for (auto const & [event, data] : m_sseFrames)
                {
                    onEvent(event, data);
                }
                co_return HttpResponse{200, "", "", false};
            }

          public:
            std::string capturedBody;

          private:
            std::string m_jsonBody;
            std::vector<std::pair<std::string, std::string>> m_sseFrames;
            std::chrono::milliseconds m_delay{0};
        };

        [[nodiscard]] CompletionRequest simpleRequest()
        {
            CompletionRequest req;
            req.systemPrompt = "Design";
            Conversation conv;
            conv.appendUserText("Make a sphere");
            req.messages = conv.messages();
            return req;
        }

        constexpr char const * const kAnthropicJson =
          R"({"id":"x","content":[{"type":"text","text":"done"}],"stop_reason":"end_turn",
              "usage":{"input_tokens":120,"output_tokens":34,
                      "cache_creation_input_tokens":7,"cache_read_input_tokens":900}})";

        constexpr char const * const kOpenAiJson =
          R"({"choices":[{"index":0,"finish_reason":"stop",
              "message":{"role":"assistant","content":"done"}}],
              "usage":{"prompt_tokens":210,"completion_tokens":42,
                       "prompt_tokens_details":{"cached_tokens":64}}})";
    } // namespace

    TEST(AnthropicProvider, NonStreamingUsage_ReportedOnResponse)
    {
        auto transport = std::make_shared<MockTransport>(kAnthropicJson,
                                                        std::vector<std::pair<std::string, std::string>>{});
        ProviderConfig cfg;
        cfg.stream = false;
        AnthropicProvider provider{cfg, transport};

        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.usage.inputTokens, 120);
        EXPECT_EQ(resp.usage.outputTokens, 34);
        EXPECT_EQ(resp.usage.cacheWriteTokens, 7);
        EXPECT_EQ(resp.usage.cacheReadTokens, 900);
        EXPECT_EQ(resp.usage.totalPromptTokens(), 1027);
        EXPECT_TRUE(resp.usage.isReported());
    }

    TEST(AnthropicProvider, StreamingUsage_MergedFromMessageStartAndDelta)
    {
        std::vector<std::pair<std::string, std::string>> frames = {
          {"message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":100,"output_tokens":1,"cache_read_input_tokens":50}}})"},
          {"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
          {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hi"}})"},
          {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":25}})"}};
        auto transport = std::make_shared<MockTransport>("", std::move(frames));
        ProviderConfig cfg;
        cfg.stream = true;
        AnthropicProvider provider{cfg, transport};

        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.text, "hi");
        EXPECT_EQ(resp.stopReason, "tool_use");
        EXPECT_EQ(resp.usage.inputTokens, 100) << "Prompt totals survive the later delta frame";
        EXPECT_EQ(resp.usage.cacheReadTokens, 50);
        EXPECT_EQ(resp.usage.outputTokens, 25);
    }

    TEST(OpenAiProvider, NonStreamingUsage_ReportedOnResponse)
    {
        auto transport = std::make_shared<MockTransport>(kOpenAiJson,
                                                        std::vector<std::pair<std::string, std::string>>{});
        ProviderConfig cfg;
        cfg.stream = false;
        OpenAiProvider provider{cfg, transport};

        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.usage.inputTokens, 210);
        EXPECT_EQ(resp.usage.outputTokens, 42);
        EXPECT_EQ(resp.usage.cacheReadTokens, 64);
    }

    TEST(OpenAiProvider, StreamingUsage_ReadFromUsageOnlyChunk)
    {
        std::vector<std::pair<std::string, std::string>> frames = {
          {"", R"({"choices":[{"index":0,"delta":{"content":"hi"}}]})"},
          {"", R"({"choices":[],"usage":{"prompt_tokens":77,"completion_tokens":9}})"},
          {"", "[DONE]"}};
        auto transport = std::make_shared<MockTransport>("", std::move(frames));
        ProviderConfig cfg;
        cfg.stream = true;
        OpenAiProvider provider{cfg, transport};

        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.text, "hi");
        EXPECT_EQ(resp.usage.inputTokens, 77);
        EXPECT_EQ(resp.usage.outputTokens, 9);
    }

    TEST(OpenAiProvider, StreamingPayloadRequestsUsageInclusion)
    {
        auto transport = std::make_shared<MockTransport>("",
                                                        std::vector<std::pair<std::string, std::string>>{});
        ProviderConfig cfg;
        OpenAiProvider provider{cfg, transport};

        coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        auto const body = nlohmann::json::parse(transport->capturedBody);
        ASSERT_TRUE(body.contains("stream_options"));
        EXPECT_EQ(body["stream_options"]["include_usage"], true);
    }

    TEST(AnthropicProvider, StreamingTiming_SplitsPrefillFromGeneration)
    {
        std::vector<std::pair<std::string, std::string>> frames = {
          {"message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":100}}})"},
          {"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking"}})"},
          {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"reasoning"}})"},
          {"content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"c1","name":"set_function_snippet"}})"},
          {"content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"a\":1}"}})"},
          {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":25}})"}};
        auto const frameCount = frames.size();
        auto transport = std::make_shared<MockTransport>("", std::move(frames));
        transport->withDelay(std::chrono::milliseconds(15));
        ProviderConfig cfg;
        cfg.stream = true;
        AnthropicProvider provider{cfg, transport};

        // Act
        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        // Assert: reasoning is counted even though it never reaches the conversation
        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_GT(resp.timing.ttftMs, 0) << "Time to first token is what prefill costs";
        EXPECT_GE(resp.timing.firstContentMs, resp.timing.ttftMs);
        EXPECT_GE(resp.timing.totalMs, resp.timing.firstContentMs);
        EXPECT_EQ(resp.timing.streamEvents, frameCount);
        EXPECT_EQ(resp.timing.thinkingChars, std::string{"reasoning"}.size());
        EXPECT_EQ(resp.timing.toolJsonChars, std::string{R"({"a":1})"}.size());
        EXPECT_EQ(resp.timing.textChars, 0u);
    }

    TEST(AnthropicProvider, NonStreamingTiming_HasNoPrefillPhase)
    {
        auto transport = std::make_shared<MockTransport>(kAnthropicJson,
                                                        std::vector<std::pair<std::string, std::string>>{});
        transport->withDelay(std::chrono::milliseconds(15));
        ProviderConfig cfg;
        cfg.stream = false;
        AnthropicProvider provider{cfg, transport};

        // Act
        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        // Assert: without a stream there is nothing to split, so all marks coincide
        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_GT(resp.timing.totalMs, 0);
        EXPECT_EQ(resp.timing.ttftMs, resp.timing.totalMs);
        EXPECT_EQ(resp.timing.firstContentMs, resp.timing.totalMs);
        EXPECT_EQ(resp.timing.streamEvents, 0u);
    }

    TEST(OpenAiProvider, StreamingTiming_CountsReasoningContent)
    {
        std::vector<std::pair<std::string, std::string>> frames = {
          {"", R"({"choices":[{"index":0,"delta":{"reasoning_content":"thoughts"}}]})"},
          {"", R"({"choices":[{"index":0,"delta":{"content":"hi","role":"assistant"}}]})"},
          {"", R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})"},
          {"", R"({"usage":{"prompt_tokens":210,"completion_tokens":42}})"}};
        auto transport = std::make_shared<MockTransport>("", std::move(frames));
        transport->withDelay(std::chrono::milliseconds(15));
        ProviderConfig cfg;
        cfg.stream = true;
        OpenAiProvider provider{cfg, transport};

        // Act
        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        // Assert
        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.text, "hi");
        EXPECT_GT(resp.timing.ttftMs, 0);
        EXPECT_GE(resp.timing.totalMs, resp.timing.ttftMs);
        EXPECT_EQ(resp.timing.thinkingChars, std::string{"thoughts"}.size());
        EXPECT_EQ(resp.timing.textChars, std::string{"hi"}.size());
    }

    TEST(FallbackProvider, SuccessfulCandidate_StampsProviderLabel)
    {
        auto transport = std::make_shared<MockTransport>(kAnthropicJson,
                                                        std::vector<std::pair<std::string, std::string>>{});
        auto inner = std::make_unique<AnthropicProvider>(ProviderConfig{.stream = false}, transport);

        std::vector<FallbackProvider::Candidate> candidates;
        candidates.push_back(FallbackProvider::Candidate{"strata/iq3_s", std::move(inner)});
        FallbackProvider provider{std::move(candidates)};
        auto const resp = coro::sync_wait(provider.complete(simpleRequest(), nullptr, {}));

        ASSERT_TRUE(resp.success) << resp.error;
        EXPECT_EQ(resp.providerName, "strata/iq3_s");
        EXPECT_EQ(resp.usage.outputTokens, 34);
    }
} // namespace gladius::agent::tests
