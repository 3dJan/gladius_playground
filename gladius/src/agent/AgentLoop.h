#pragma once

#include "AgentEvent.h"
#include "AgentLimitKind.h"
#include "AgentRunControl.h"
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
#include <optional>
#include <string>
#include <utility>

namespace gladius::agent
{
    /// @brief Hard execution limits preventing runaway cost or infinite refinement
    /// @details A run is unlimited on a dimension by default: long design sessions with local
    ///          models are the normal case, and a limit that fires parks the run so the user can
    ///          continue it instead of failing it.
    struct AgentLimits
    {
        /// @brief Model-turn budget per segment; 0 or less means unlimited
        int maxModelTurns = 0;
        /// @brief Tool-call budget per segment; 0 means unlimited
        std::size_t maxToolCalls = 0;
        /// @brief Viewport images a segment may request; 0 means unlimited
        std::size_t maxViewportReviews = 0;
        /// @brief Wall-clock budget per segment; 0 or less means unlimited
        std::chrono::milliseconds maxWallTime{0};
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
        /// @brief Model turns that returned neither text nor tool calls
        std::size_t emptyTurns = 0;
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
        /// @brief Reports a host-side limit the loop does not count itself, with the amount used
        using SecondaryLimitProbe =
          std::function<std::optional<std::pair<AgentLimitKind, std::size_t>>()>;

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
          trace::AgentTraceRecorder * traceRecorder = nullptr,
          AgentRunControl * runControl = nullptr,
          SecondaryLimitProbe secondaryLimitProbe = nullptr);
    };
} // namespace gladius::agent
