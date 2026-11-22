#pragma once

#include "../Conversation.h"
#include "../../io/CancellationToken.h"
#include "../../tools/ToolDefinition.h"

#include <coro/coro.hpp>
#include <functional>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Configuration for an AI reasoning or image provider
    struct ProviderConfig
    {
        std::string endpoint;
        std::string apiKey;
        std::string model;
        int maxTokens = 4096;
        double temperature = 0.2;
        bool stream = true;
        std::vector<std::string> extraHeaders;
    };

    /// @brief Provider-neutral completion request
    struct CompletionRequest
    {
        std::string systemPrompt;
        std::vector<Message> messages;
        std::vector<tools::ToolDefinition> tools;
    };

    /// @brief Provider-neutral completion response
    struct CompletionResponse
    {
        std::string text;
        std::vector<ToolCall> toolCalls;
        std::string stopReason;
        bool success = true;
        std::string error;
    };

    /// @brief Streaming text delta callback
    using TextDeltaCallback = std::function<void(std::string const & delta)>;

    /// @brief Abstract interface for LLM providers (OpenAI-compatible, Anthropic, etc.)
    class IAiProvider
    {
      public:
        virtual ~IAiProvider() = default;

        [[nodiscard]] virtual std::string name() const = 0;

        [[nodiscard]] virtual coro::task<CompletionResponse>
        complete(CompletionRequest const & request,
                 TextDeltaCallback onDelta,
                 io::CancellationToken const & cancellation) = 0;
    };
} // namespace gladius::agent
