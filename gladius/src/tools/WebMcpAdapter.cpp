#include "WebMcpAdapter.h"

namespace gladius::tools
{
    WebMcpAdapter::WebMcpAdapter(ToolRegistry const & registry)
        : m_registry(registry)
        , m_executor(registry)
    {
    }

    nlohmann::json WebMcpAdapter::listToolsJson() const
    {
        nlohmann::json out = nlohmann::json::array();
        for (auto const & def : m_registry.definitions())
        {
            out.push_back({
                {"name", def.name},
                {"description", def.description},
                {"inputSchema", def.inputSchema}
            });
        }
        return out;
    }

    nlohmann::json WebMcpAdapter::invokeToolJson(std::string const & name,
                                                 nlohmann::json const & arguments) const
    {
        io::CancellationToken noCancel;
        return m_executor.executeSync(name, arguments, noCancel);
    }
} // namespace gladius::tools
