#include "agent/CredentialStore.h"
#include "agent/providers/AnthropicProvider.h"
#include "agent/providers/ModelDiscovery.h"
#include "agent/providers/OpenAiProvider.h"
#include "agent/providers/ProviderProfile.h"

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

            HttpResponse getJson(HttpRequest const & request,
                                 io::CancellationToken const & /*cancellation*/) override
            {
                lastRequest = request;
                return nextResponse;
            }

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

    TEST(ModelDiscovery, FetchModels_OpenRouterWithoutApiKey_ReturnsOnlyFreeModels)
    {
        // Arrange
        MockHttpTransport mock;
        mock.nextResponse = HttpResponse{
          200,
          R"({"data":[
                {"id":"openai/gpt-4o","pricing":{"prompt":"0.000005","completion":"0.000015"}},
                {"id":"meta-llama/llama-3.3-70b-instruct:free","pricing":{"prompt":"0","completion":"0"}},
                {"id":"google/gemma-3-27b-it:free","pricing":{"prompt":"0","completion":"0"}}
              ]})",
          "",
          false};

        ProviderProfile prof{
          "or", "OpenRouter", ApiType::OpenAiCompatible, "https://openrouter.ai/api/v1", {}, ""};
        io::CancellationToken cancel;

        // Act
        auto const res = ModelDiscovery::fetchModels(prof, mock, cancel);

        // Assert
        EXPECT_TRUE(res.success);
        ASSERT_EQ(res.models.size(), 2u);
        EXPECT_EQ(res.models[0], "meta-llama/llama-3.3-70b-instruct:free");
        EXPECT_EQ(res.models[1], "google/gemma-3-27b-it:free");
    }

    TEST(ModelDiscovery, FetchModels_OpenRouterWithApiKeyAndFreeOnly_ExcludesPaidModelsAndParsesMetadata)
    {
        // Arrange
        MockHttpTransport mock;
        mock.nextResponse = HttpResponse{
          200,
          R"({"data":[
                {"id":"openai/gpt-4o","context_length":128000,
                 "architecture":{"input_modalities":["text","image"]},
                 "pricing":{"prompt":"0.0000025","completion":"0.00001"}},
                {"id":"google/gemma-3-27b-it:free","context_length":96000,
                 "architecture":{"input_modalities":["text","image"]},
                 "pricing":{"prompt":"0","completion":"0"}}
              ]})",
          "",
          false};

        ProviderProfile prof{
          "or",
          "OpenRouter",
          ApiType::OpenAiCompatible,
          "https://openrouter.ai/api/v1",
          {},
          "sk-or-test"};
        io::CancellationToken cancel;

        // Act
        auto const freeRes = ModelDiscovery::fetchModels(prof, mock, cancel, true);
        auto const allRes = ModelDiscovery::fetchModels(prof, mock, cancel, false);

        // Assert
        ASSERT_TRUE(freeRes.success);
        ASSERT_EQ(freeRes.models.size(), 1u);
        EXPECT_EQ(freeRes.models[0], "google/gemma-3-27b-it:free");
        EXPECT_TRUE(freeRes.metadata[0].isFree);
        EXPECT_TRUE(freeRes.metadata[0].supportsVision);
        EXPECT_EQ(freeRes.metadata[0].contextLength, 96000);

        ASSERT_TRUE(allRes.success);
        ASSERT_EQ(allRes.models.size(), 2u);
        EXPECT_NEAR(allRes.metadata[0].promptPricePerMillion, 2.5, 1e-6);
        EXPECT_NEAR(allRes.metadata[0].completionPricePerMillion, 10.0, 1e-6);
        EXPECT_TRUE(allRes.metadata[0].supportsVision);
    }

    TEST(ProviderProfile, IsModelAvailable_LocalAndApiKeyAndFreeOpenRouter_FiltersCorrectly)
    {
        // Arrange
        ProviderProfile local{
          "loc", "Local", ApiType::OpenAiCompatible, "http://localhost:11434/v1", {"qwen2.5"}, ""};
        ProviderProfile remoteNoKey{
          "rem", "Remote", ApiType::OpenAiCompatible, "https://api.openai.com/v1", {"gpt-4o"}, ""};
        ProviderProfile orNoKey{
          "or", "OpenRouter", ApiType::OpenAiCompatible, "https://openrouter.ai/api/v1", {}, ""};

        // Assert
        EXPECT_TRUE(local.isModelAvailable("qwen2.5"));
        EXPECT_FALSE(remoteNoKey.isModelAvailable("gpt-4o"));
        EXPECT_TRUE(orNoKey.isModelAvailable("google/gemma-3-27b-it:free"));
        EXPECT_FALSE(orNoKey.isModelAvailable("openai/gpt-4o"));
    }

    TEST(OsCredentialStore, StoreLoadDelete_RoundTripsSecret)
    {
        // Arrange
        OsCredentialStore store;
        std::string const service = "gladius.unittest";
        std::string const account = "test_openrouter_key";
        std::string const secret = "sk-or-v1-test-secret-12345";

        // Act
        ASSERT_TRUE(store.storeSecret(service, account, secret));
        auto const loaded = store.loadSecret(service, account);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(*loaded, secret);

        // Cleanup & verify delete
        EXPECT_TRUE(store.deleteSecret(service, account));
        EXPECT_FALSE(store.loadSecret(service, account).has_value());
    }
} // namespace gladius::agent::tests
