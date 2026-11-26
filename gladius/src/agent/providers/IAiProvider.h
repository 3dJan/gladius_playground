#pragma once

#include "../Conversation.h"
#include "ProviderError.h"
#include "../../io/CancellationToken.h"
#include "../../tools/ToolDefinition.h"

#include <coro/coro.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Where the explicit prompt-cache breakpoints are placed in a request
    /// @details A provider only reads back the prompt up to a breakpoint, so the placement decides
    ///          how much of a growing conversation is reused between turns.
    enum class CacheBreakpointPlacement
    {
        /// @brief Send no breakpoint and let the server cache the prefix on its own
        None,
        /// @brief Breakpoint after the tool schemas only: caches system prompt and tools
        Tools,
        /// @brief Breakpoint after the tool schemas and after the newest message: caches everything
        CacheAll,
    };

    /// @brief Configuration for an AI reasoning or image provider
    struct ProviderConfig
    {
        std::string endpoint;
        std::string apiKey;
        std::string model;
        int maxTokens = 4096;
        /// @brief Token cap for the model's hidden reasoning; `<= 0` leaves it to the provider
        /// @details Reasoning costs the same per token as visible output but is never reused, so an
        ///          unbounded budget on a reasoning model dominates the wall time of every turn.
        int thinkingBudgetTokens = 0;
        double temperature = 0.2;
        /// @brief Explicit prompt-cache breakpoints; local servers usually cache the prefix on their
        ///          own and charge an extra cache write per turn for an explicit breakpoint
        CacheBreakpointPlacement cacheBreakpoints = CacheBreakpointPlacement::Tools;
        bool stream = true;
        std::vector<std::string> extraHeaders;
        /// @brief Per-request HTTP timeout; local models need far more than cloud endpoints
        int timeoutSeconds = 120;
    };

    /// @brief Provider-neutral completion request
    struct CompletionRequest
    {
        std::string systemPrompt;
        std::vector<Message> messages;
        std::vector<tools::ToolDefinition> tools;
    };

    /// @brief Token accounting of a single completion, as reported by the provider
    struct TokenUsage
    {
        /// @brief Tokens billed for the prompt, excluding cached reads and cache writes
        std::int64_t inputTokens = 0;
        std::int64_t outputTokens = 0;
        /// @brief Prompt tokens served from the provider prompt cache
        std::int64_t cacheReadTokens = 0;
        /// @brief Prompt tokens written into the provider prompt cache
        std::int64_t cacheWriteTokens = 0;

        /// @brief Prompt tokens of the request independent of how they were billed
        [[nodiscard]] std::int64_t totalPromptTokens() const noexcept
        {
            return inputTokens + cacheReadTokens + cacheWriteTokens;
        }

        [[nodiscard]] bool isReported() const noexcept
        {
            return totalPromptTokens() > 0 || outputTokens > 0;
        }

        /// @brief Add the tokens of another completion to this total
        void accumulate(TokenUsage const & other) noexcept
        {
            inputTokens += other.inputTokens;
            outputTokens += other.outputTokens;
            cacheReadTokens += other.cacheReadTokens;
            cacheWriteTokens += other.cacheWriteTokens;
        }
    };

    /**
     * @brief Wall-clock and output-shape timing of one completion, measured by the provider.
     * @details Token counts alone cannot tell a slow endpoint from an oversized prompt: `ttftMs`
     *          covers queueing and prefill while the remainder of `totalMs` is generation, and the
     *          character counters say what that generation actually was.
     */
    struct CompletionTiming
    {
        /// @brief Milliseconds from request start to the first streamed event (0 when unmeasured)
        std::int64_t ttftMs = 0;
        /// @brief Milliseconds from request start to the first generated token, reasoning included
        /// @details Prefill really ends here: a reasoning model starts producing output long before
        ///          any of it becomes visible, so `firstContentMs` alone measures reasoning time too.
        std::int64_t firstTokenMs = 0;
        /// @brief Milliseconds from request start to the first visible content (text or tool args)
        std::int64_t firstContentMs = 0;
        /// @brief Milliseconds from request start to the final event
        std::int64_t totalMs = 0;
        /// @brief Streamed SSE events seen; a low count with a long total means the server stalled
        std::size_t streamEvents = 0;
        /// @brief Characters the model spent on reasoning it will not reuse
        std::size_t thinkingChars = 0;
        /// @brief Characters of assistant text
        std::size_t textChars = 0;
        /// @brief Characters of tool-call argument JSON
        std::size_t toolJsonChars = 0;
    };

    /// @brief Provider-neutral completion response
    struct CompletionResponse
    {
        std::string text;
        std::vector<ToolCall> toolCalls;
        std::string stopReason;
        bool success = true;
        std::string error;
        /// @brief Classified `error`; empty when the provider could not classify it
        ProviderError errorInfo;
        /// @brief Empty (`isReported() == false`) when the endpoint reports no usage
        TokenUsage usage;
        /// @brief Provider that produced the response; set by FallbackProvider routing
        std::string providerName;
        /// @brief Measured by the provider; zero when the endpoint path does not instrument it
        CompletionTiming timing;
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
