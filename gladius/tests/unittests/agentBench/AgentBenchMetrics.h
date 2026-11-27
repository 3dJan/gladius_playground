#pragma once

#include "HeadlessAgentHost.h"
#include "agent/AgentEvent.h"
#include "agent/providers/IAiProvider.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gladius::agent::bench
{
    /// @brief Per-tool call/failure counters
    struct ToolCounter
    {
        std::size_t calls = 0;
        std::size_t failures = 0;
    };

    /// @brief Harness metrics of one agent run; the quantitative part of the benchmark
    struct AgentBenchMetrics
    {
        std::string taskId;
        std::int64_t wallMs = 0;
        std::size_t modelTurns = 0;
        std::size_t toolCalls = 0;
        std::size_t toolFailures = 0;
        std::size_t unknownToolCalls = 0;
        std::size_t providerFailures = 0;
        /// @brief Model turns cut off at the provider output limit
        std::size_t truncatedTurns = 0;
        /// @brief Model turns that came back with neither text nor tool calls
        std::size_t emptyTurns = 0;
        std::size_t viewportCaptures = 0;
        /// @brief `ask_user` calls the model made (the tool itself caps how many get through)
        std::size_t userQuestionsAsked = 0;
        /// @brief Questions that produced a usable answer, assumed recommendations included
        std::size_t userQuestionsAnswered = 0;
        /// @brief How each question was resolved, oldest first; empty when nothing was asked
        std::vector<agent::ScriptedDecision> decisions;
        std::size_t librarySearches = 0;
        std::size_t libraryImports = 0;
        std::size_t libraryExports = 0;
        std::size_t designGraphMutations = 0;
        bool agentCompleted = false;
        std::string agentError;
        std::string finalMessage;
        TokenUsage usage;
        std::string providerName;
        std::map<std::string, ToolCounter> perTool;
        std::vector<std::string> failureDetails;
        /// @brief Library files that appeared in the sandbox user library during the run
        std::vector<std::string> libraryFilesAdded;

        nlohmann::json toJson() const;
    };

    /// @brief Provider-failure tally that outlives the provider it observes
    struct ProviderFailureCounter
    {
        std::size_t failures = 0;
    };

    /// @brief Decorator that observes provider-level failures and the active provider name
    class ObservingProvider final : public IAiProvider
    {
      public:
        ObservingProvider(std::unique_ptr<IAiProvider> inner,
                          std::string label,
                          std::shared_ptr<ProviderFailureCounter> counter = {})
            : m_inner(std::move(inner))
            , m_label(std::move(label))
            , m_counter(counter ? std::move(counter) : std::make_shared<ProviderFailureCounter>())
        {
        }

        [[nodiscard]] std::string name() const override
        {
            return m_inner->name();
        }

        coro::task<CompletionResponse> complete(CompletionRequest const & request,
                                                TextDeltaCallback onDelta,
                                                io::CancellationToken const & cancellation) override
        {
            auto resp = co_await m_inner->complete(request, onDelta, cancellation);
            if (resp.providerName.empty())
            {
                resp.providerName = m_label;
            }
            if (!resp.success)
            {
                ++m_counter->failures;
            }
            co_return resp;
        }

        [[nodiscard]] std::size_t providerFailures() const noexcept
        {
            return m_counter->failures;
        }

      private:
        std::unique_ptr<IAiProvider> m_inner;
        std::string m_label;
        std::shared_ptr<ProviderFailureCounter> m_counter;
    };

    /// @brief Aggregates agent events plus run result into `AgentBenchMetrics`
    class AgentBenchMetricsCollector
    {
      public:
        explicit AgentBenchMetricsCollector(std::string taskId);

        /// @brief Fold one drained agent event into the metrics
        void record(AgentEvent const & event);

        /// @brief Fold all events drained from the host, then merge the run result
        void collectFrom(HeadlessAgentHost & host,
                         AgentRunResult const & result,
                         std::size_t providerFailures);

        /// @brief Counts files in the sandbox user library that were not present before the run
        void diffUserLibrary(std::filesystem::path const & userLibraryDir,
                             std::vector<std::string> const & filesBeforeRun);

        [[nodiscard]] AgentBenchMetrics const & metrics() const noexcept
        {
            return m_metrics;
        }

      private:
        void recordToolCompletion(std::string const & name,
                                  nlohmann::json const & result,
                                  nlohmann::json const & arguments,
                                  bool success);

        AgentBenchMetrics m_metrics;
        std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();
        std::map<std::string, nlohmann::json> m_pendingArguments;
        std::set<std::string> m_importedEntries;
        std::set<std::string> m_exportedEntries;
    };

    /// @brief Lists the .3mf files of a library directory as stable relative paths
    [[nodiscard]] std::vector<std::string> listLibraryFiles(std::filesystem::path const & dir);
} // namespace gladius::agent::bench
