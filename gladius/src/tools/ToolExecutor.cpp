#include "ToolExecutor.h"

#include <exception>

namespace gladius::tools
{
    ToolExecutor::ToolExecutor(ToolRegistry const & registry, PolicyCheck policyCheck)
        : m_registry(registry)
        , m_policyCheck(std::move(policyCheck))
    {
    }

    coro::task<nlohmann::json> ToolExecutor::execute(std::string const & toolName,
                                                     nlohmann::json const & arguments,
                                                     io::CancellationToken const & cancellation) const
    {
        if (cancellation.isCancelled())
        {
            co_return nlohmann::json{
                {"success", false},
                {"error", "Execution cancelled before tool start"},
                {"tool", toolName}};
        }

        auto tool = m_registry.getTool(toolName);
        if (!tool)
        {
            co_return nlohmann::json{
                {"success", false},
                {"error", "Unknown tool: " + toolName},
                {"tool", toolName}};
        }

        ToolDefinition const def = tool->definition();
        if (m_policyCheck)
        {
            PolicyDecision const decision = m_policyCheck(def, arguments);
            if (!decision.allowed)
            {
                co_return nlohmann::json{
                    {"success", false},
                    {"error", decision.reason.empty() ? "Tool execution denied by policy" : decision.reason},
                    {"policy", toString(def.policy)},
                    {"tool", toolName}};
            }
        }

        try
        {
            co_return co_await tool->execute(arguments, cancellation);
        }
        catch (std::exception const & ex)
        {
            co_return nlohmann::json{
                {"success", false},
                {"error", ex.what()},
                {"tool", toolName}};
        }
        catch (...)
        {
            co_return nlohmann::json{
                {"success", false},
                {"error", "Unknown exception during tool execution"},
                {"tool", toolName}};
        }
    }

    nlohmann::json ToolExecutor::executeSync(std::string const & toolName,
                                             nlohmann::json const & arguments,
                                             io::CancellationToken const & cancellation) const
    {
        return coro::sync_wait(execute(toolName, arguments, cancellation));
    }
} // namespace gladius::tools
