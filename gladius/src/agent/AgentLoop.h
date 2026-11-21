#pragma once

#include "AgentEvent.h"
#include "ContextBuilder.h"
#include "Conversation.h"
#include "PermissionPolicy.h"
#include "providers/IAiProvider.h"
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
        std::size_t maxModelTurns = 64;
        std::size_t maxToolCalls = 256;
        std::size_t maxViewportReviews = 16;
        std::chrono::minutes maxWallTime{15};
    };

    /// @brief Outcome of an AgentLoop execution
    struct AgentRunResult
    {
        bool completed = false;
        bool cancelled = false;
        std::size_t modelTurns = 0;
        std::size_t toolCalls = 0;
        std::string finalMessage;
        std::string error;
    };

    /// @brief Bounded, policy-enforced iterative agent loop
    class AgentLoop
    {
      public:
        using CompletionPredicate = std::function<bool()>;
        using ConfirmationCallback =
          std::function<bool(tools::ToolDefinition const &, nlohmann::json const &)>;
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
          SnapshotProvider snapshotProvider = nullptr);
    };
} // namespace gladius::agent
