#pragma once

#include "ToolExecutor.h"
#include "ToolRegistry.h"

#include <nlohmann/json.hpp>
#include <string>

namespace gladius::tools
{
    /// @brief Adapter exposing the canonical ToolRegistry to WebMCP / browser JS bridges
    class WebMcpAdapter
    {
      public:
        explicit WebMcpAdapter(ToolRegistry const & registry);

        /// @brief Return WebMCP-compatible tool descriptors (name, description, inputSchema)
        [[nodiscard]] nlohmann::json listToolsJson() const;

        /// @brief Invoke a registered tool from a JSON arguments payload
        [[nodiscard]] nlohmann::json invokeToolJson(std::string const & name,
                                                    nlohmann::json const & arguments) const;

      private:
        ToolRegistry const & m_registry;
        ToolExecutor m_executor;
    };
} // namespace gladius::tools
