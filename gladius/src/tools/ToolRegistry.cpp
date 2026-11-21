#include "ToolRegistry.h"

#include <stdexcept>

namespace gladius::tools
{
    void ToolRegistry::registerTool(std::shared_ptr<ITool> tool)
    {
        if (!tool)
        {
            throw std::invalid_argument("Cannot register null tool");
        }
        ToolDefinition def = tool->definition();
        if (def.name.empty())
        {
            throw std::invalid_argument("Cannot register tool with empty name");
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tools[def.name] = std::move(tool);
    }

    void ToolRegistry::registerSyncTool(ToolDefinition definition, SyncFunctionTool::Callback callback)
    {
        registerTool(std::make_shared<SyncFunctionTool>(std::move(definition), std::move(callback)));
    }

    bool ToolRegistry::unregisterTool(std::string const & name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tools.erase(name) > 0;
    }

    bool ToolRegistry::hasTool(std::string const & name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tools.find(name) != m_tools.end();
    }

    std::shared_ptr<ITool> ToolRegistry::getTool(std::string const & name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tools.find(name);
        if (it == m_tools.end())
        {
            return nullptr;
        }
        return it->second;
    }

    std::vector<ToolDefinition> ToolRegistry::definitions() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ToolDefinition> result;
        result.reserve(m_tools.size());
        for (auto const & [name, tool] : m_tools)
        {
            result.push_back(tool->definition());
        }
        return result;
    }

    std::vector<ToolDefinition> ToolRegistry::definitionsByMaxPolicy(ToolPolicy maxPolicy) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ToolDefinition> result;
        for (auto const & [name, tool] : m_tools)
        {
            ToolDefinition def = tool->definition();
            if (static_cast<int>(def.policy) <= static_cast<int>(maxPolicy))
            {
                result.push_back(std::move(def));
            }
        }
        return result;
    }

    std::size_t ToolRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tools.size();
    }
} // namespace gladius::tools
