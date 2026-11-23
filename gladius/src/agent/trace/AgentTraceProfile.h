#pragma once

#include "AgentTrace.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::agent::trace
{
    /// @brief One tool's share of a run, ranked by total wall time
    struct ToolProfileEntry
    {
        std::string name;
        std::size_t calls = 0;
        std::size_t failures = 0;
        std::int64_t totalMs = 0;
        std::int64_t maxMs = 0;
        std::int64_t totalResultBytes = 0;

        [[nodiscard]] nlohmann::json toJson() const;
    };

    /**
     * @brief Aggregated view of one run's trace: where the time and the tokens went.
     * @details Built from the same records as the JSONL trace so the aggregate and the raw trace can
     *          never disagree.
     */
    struct AgentTraceProfile
    {
        std::int64_t wallMs = 0;
        std::size_t turns = 0;
        std::size_t toolCalls = 0;

        // Time split. `unaccountedMs` is explicit so a missing span is visible instead of silently
        // folded into model time.
        std::int64_t contextBuildMs = 0;
        std::int64_t modelMs = 0;
        /// @brief Part of `modelMs` spent prefilling the prompt, i.e. before the first output token
        std::int64_t modelPrefillMs = 0;
        /// @brief Part of `modelMs` spent generating output tokens
        std::int64_t modelGenerateMs = 0;
        std::int64_t toolMs = 0;
        std::int64_t harnessMs = 0;
        std::int64_t unaccountedMs = 0;

        // Model turn timings
        std::int64_t turnDurationP50Ms = 0;
        std::int64_t turnDurationP95Ms = 0;
        std::int64_t turnDurationMaxMs = 0;
        std::int64_t ttftP50Ms = 0;
        std::int64_t ttftP95Ms = 0;
        double meanDecodeTokensPerSec = 0.0;
        double meanPrefillTokensPerSec = 0.0;
        std::size_t thinkingChars = 0;
        std::size_t textChars = 0;
        std::size_t toolJsonChars = 0;

        // Context composition and growth
        std::int64_t firstTurnPromptTokens = 0;
        std::int64_t lastTurnPromptTokens = 0;
        std::int64_t peakTurnPromptTokens = 0;
        std::int64_t totalPromptTokens = 0;
        /// @brief Re-sent history: sum of per-turn prompt tokens divided by the final turn's prompt
        double contextTax = 0.0;
        /// @brief Share of prompt tokens the provider served from its prompt cache
        double promptCacheHitRate = 0.0;
        /// @brief Turns whose system prompt hash differs from the previous turn, i.e. turns that threw
        ///        away the cached prefix
        std::size_t systemPromptMutations = 0;
        /// @brief Turns the provider reported no cached prompt tokens for
        std::size_t cacheMissTurns = 0;
        /// @brief Viewport images actually sent in the last turn
        std::size_t imagesInHistoryLastTurn = 0;
        std::size_t imagesKept = 0;
        std::size_t imagesDropped = 0;
        std::size_t compactedToolResults = 0;
        nlohmann::json promptBytesByComponent = nlohmann::json::object();
        std::int64_t largestToolResultBytes = 0;
        std::size_t largestToolResultTurn = 0;

        std::vector<ToolProfileEntry> tools;

        // Waste attribution
        std::size_t truncatedTurns = 0;
        std::size_t failedToolCalls = 0;
        std::size_t deniedToolCalls = 0;
        std::size_t turnsAfterFailure = 0;
        std::int64_t turnsAfterFailureMs = 0;
        std::int64_t turnsAfterFailurePromptTokens = 0;
        /// @brief Cost of the turns that issued no tool call (only the closing turn is expected)
        std::int64_t readOnlyTurnsMs = 0;
        std::int64_t judgeMs = 0;
        /// @brief Turns that issued no tool call; exactly one is expected (the closing turn)
        std::size_t turnsWithoutToolCalls = 0;
        /// @brief Tool calls that changed the design graph or the model
        std::size_t mutatingToolCalls = 0;
        std::size_t autonomousNudges = 0;
        std::size_t truncationNudges = 0;

        [[nodiscard]] nlohmann::json toJson() const;
    };

    /**
     * @brief Aggregates trace records into an `AgentTraceProfile`.
     * @param records the records held by an `AgentTraceRecorder`
     * @param wallMs total run wall time; when non-positive the trace extent is used instead
     */
    [[nodiscard]] AgentTraceProfile analyzeTrace(std::vector<nlohmann::json> const & records,
                                                 std::int64_t wallMs = 0);

    /// @brief Human-readable ranking of the biggest costs, for logs and stdout
    [[nodiscard]] std::string formatProfileSummary(AgentTraceProfile const & profile);
} // namespace gladius::agent::trace
