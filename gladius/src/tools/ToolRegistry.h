#pragma once

#include "ITool.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gladius::tools
{
    /// @brief Thread-safe canonical registry of Gladius tools
    class ToolRegistry
    {
      public:
        ToolRegistry() = default;
        ~ToolRegistry() = default;

        ToolRegistry(ToolRegistry const &) = delete;
        ToolRegistry & operator=(ToolRegistry const &) = delete;
        ToolRegistry(ToolRegistry &&) = delete;
        ToolRegistry & operator=(ToolRegistry &&) = delete;

        /// @brief Register an ITool instance (replaces any existing tool with the same name)
        void registerTool(std::shared_ptr<ITool> tool);

        /// @brief Register a synchronous callback as an ITool
        void registerSyncTool(ToolDefinition definition, SyncFunctionTool::Callback callback);

        /// @brief Remove a tool by name; returns true if it existed
        bool unregisterTool(std::string const & name);

        /// @brief Check whether a tool with the given name is registered
        [[nodiscard]] bool hasTool(std::string const & name) const;

        /// @brief Look up a tool by name, returning nullptr if not found
        [[nodiscard]] std::shared_ptr<ITool> getTool(std::string const & name) const;

        /// @brief Return all registered tool definitions in deterministic name order
        [[nodiscard]] std::vector<ToolDefinition> definitions() const;

        /// @brief Return registered tool definitions whose policy is at or below maxPolicy
        [[nodiscard]] std::vector<ToolDefinition> definitionsByMaxPolicy(ToolPolicy maxPolicy) const;

        /// @brief Return the number of registered tools
        [[nodiscard]] std::size_t size() const;

      private:
        mutable std::mutex m_mutex;
        std::map<std::string, std::shared_ptr<ITool>> m_tools;
    };
} // namespace gladius::tools
