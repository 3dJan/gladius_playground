#pragma once

#include "AgentEvent.h"
#include "ContextBuilder.h"
#include "Conversation.h"
#include "PermissionPolicy.h"
#include "providers/IAiProvider.h"
#include "trace/AgentTrace.h"
#include "../io/CancellationToken.h"
#include "../tools/ToolExecutor.h"

#include <chrono>
#include <coro/coro.hpp>
#include <cstddef>
#include <functional>
#include <string>

namespace gladius::agent
{
    /// @brief Hard execution limits preventing runaway cost or infinite refinement
    struct AgentLimits
    {
        /// @brief Model-turn budget for a single run; a non-positive value means unlimited
        int maxModelTurns = 64;
        std::size_t maxToolCalls = 256;
        std::size_t maxViewportReviews = 16;
        /// @brief Wall-clock budget for a single run; a non-positive duration means unlimited
        std::chrono::milliseconds maxWallTime{std::chrono::minutes(15)};
    };

    /// @brief Outcome of an AgentLoop execution
    struct AgentRunResult
    {
        bool completed = false;
        bool cancelled = false;
        std::size_t modelTurns = 0;
        std::size_t toolCalls = 0;
        /// @brief Tokens reported by the provider, summed over all model turns of the run
        TokenUsage usage;
        /// @brief Provider that answered the last model turn
        std::string providerName;
        /// @brief Model turns the provider cut off at its completion limit
        std::size_t truncatedTurns = 0;
        std::string finalMessage;
        std::string error;
    };

    /// @brief Bounded, policy-enforced iterative agent loop
    class AgentLoop
    {
      public:
        using CompletionPredicate = std::function<bool()>;
        using ConfirmationCallback =
          std::function<coro::task<bool>(tools::ToolDefinition const &, nlohmann::json const &)>;
        using SnapshotProvider = std::function<DesignContextSnapshot()>;

        [[nodiscard]] coro::task<AgentRunResult> run(
          Conversation & conversation,
          IAiProvider & provider,
          tools::ToolRegistry const & registry,
          tools::ToolExecutor const & executor,
          ContextBuilder const & contextBuilder,
          PermissionPolicy const & policy,
          AgentEventQueue & eventQueue,
          AgentLimits const & limits,
          io::CancellationToken const & cancellation,
          CompletionPredicate isDesignComplete = nullptr,
          ConfirmationCallback confirmCallback = nullptr,
          SnapshotProvider snapshotProvider = nullptr,
          trace::AgentTraceRecorder * traceRecorder = nullptr);
    };
} // namespace gladius::agent
