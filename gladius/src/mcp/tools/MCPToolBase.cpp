/**
 * @file MCPToolBase.cpp
 * @brief Implementation of base class for MCP tool implementations
 */

#include "MCPToolBase.h"
#include "../../Application.h"
#include "../../Document.h"

namespace gladius::mcp::tools
{
    MCPToolBase::MCPToolBase(Application * app)
        : m_application(app)
        , m_lastErrorMessage()
    {
    }

    bool MCPToolBase::validateApplication() const
    {
        if (!m_application)
        {
            setErrorMessage("Application instance is null");
            return false;
        }
        return true;
    }

    bool MCPToolBase::validateActiveDocument() const
    {
        if (!m_application)
        {
            setErrorMessage("No active document available");
            return false;
        }

        auto document = m_application->getCurrentDocument();
        if (!document)
        {
            setErrorMessage("No active document available");
            return false;
        }
        return true;
    }

    void MCPToolBase::setErrorMessage(const std::string & message) const
    {
        m_lastErrorMessage = message;
    }

    nlohmann::json MCPToolBase::createToolError(std::string const & error,
                                                nlohmann::json const & usageExample,
                                                nlohmann::json const & additionalInfo)
    {
        nlohmann::json result;
        result["success"] = false;
        result["error"] = error;
        if (!usageExample.empty())
        {
            result["usage_example"] = usageExample;
        }
        // Merge additional info (e.g. available_categories, available_entries)
        if (additionalInfo.is_object())
        {
            for (auto const & [key, value] : additionalInfo.items())
            {
                result[key] = value;
            }
        }
        return result;
    }
} // namespace gladius::mcp::tools
