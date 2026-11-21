#pragma once

#include "ToolDefinition.h"
#include "../io/CancellationToken.h"

#include <coro/coro.hpp>
#include <functional>
#include <nlohmann/json.hpp>

namespace gladius::tools
{
    /// @brief Canonical interface for any Gladius tool usable by the native agent, MCP, or WebMCP
    class ITool
    {
      public:
        virtual ~ITool() = default;

        /// @brief Return the tool's metadata, JSON input schema, and effect policy
        [[nodiscard]] virtual ToolDefinition definition() const = 0;

        /// @brief Execute the tool asynchronously with cooperative cancellation
        [[nodiscard]] virtual coro::task<nlohmann::json>
        execute(nlohmann::json const & arguments, io::CancellationToken const & cancellation) = 0;
    };

    /// @brief Lightweight adapter wrapping a synchronous function as an ITool
    class SyncFunctionTool final : public ITool
    {
      public:
        using Callback = std::function<nlohmann::json(nlohmann::json const &)>;

        SyncFunctionTool(ToolDefinition definition, Callback callback)
            : m_definition(std::move(definition))
            , m_callback(std::move(callback))
        {
        }

        [[nodiscard]] ToolDefinition definition() const override
        {
            return m_definition;
        }

        [[nodiscard]] coro::task<nlohmann::json>
        execute(nlohmann::json const & arguments, io::CancellationToken const & cancellation) override
        {
            if (cancellation.isCancelled())
            {
                co_return nlohmann::json{{"success", false}, {"error", "Tool execution cancelled"}};
            }
            if (!m_callback)
            {
                co_return nlohmann::json{{"success", false}, {"error", "Tool has no callback"}};
            }
            co_return m_callback(arguments);
        }

      private:
        ToolDefinition m_definition;
        Callback m_callback;
    };
} // namespace gladius::tools
