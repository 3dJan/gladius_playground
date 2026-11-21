#pragma once

#include "ToolRegistry.h"

#include <coro/coro.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace gladius::tools
{
    /// @brief Decision returned by a tool execution policy check
    struct PolicyDecision
    {
        bool allowed = true;
        std::string reason;
    };

    /// @brief Callback invoked before executing a tool to enforce permission/privacy policies
    using PolicyCheck = std::function<PolicyDecision(ToolDefinition const &, nlohmann::json const &)>;

    /// @brief Executes tools from a ToolRegistry with policy enforcement, cancellation, and structured errors
    class ToolExecutor
    {
      public:
        explicit ToolExecutor(ToolRegistry const & registry, PolicyCheck policyCheck = nullptr);

        /// @brief Execute a tool asynchronously, returning structured JSON even on errors
        [[nodiscard]] coro::task<nlohmann::json> execute(std::string const & toolName,
                                                         nlohmann::json const & arguments,
                                                         io::CancellationToken const & cancellation) const;

        /// @brief Synchronous convenience wrapper for callers on non-coroutine threads
        [[nodiscard]] nlohmann::json executeSync(std::string const & toolName,
                                                 nlohmann::json const & arguments,
                                                 io::CancellationToken const & cancellation) const;

      private:
        ToolRegistry const & m_registry;
        PolicyCheck m_policyCheck;
    };
} // namespace gladius::tools
