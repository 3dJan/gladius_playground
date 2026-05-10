/**
 * @file MCPToolBase.h
 * @brief Base class for MCP tool implementations
 */

#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace gladius
{
    class Application; // Forward declaration

    namespace mcp
    {
        namespace tools
        {
            /**
             * @brief Abstract base class for MCP tool implementations
             *
             * Provides common functionality shared across all MCP tools including
             * Application reference management, error handling, and validation helpers.
             */
            class MCPToolBase
            {
              protected:
                Application * m_application; ///< Raw pointer to avoid circular dependencies
                mutable std::string m_lastErrorMessage; ///< Store detailed error information

                /// Common validation helpers
                bool validateApplication() const;
                bool validateActiveDocument() const;
                void setErrorMessage(const std::string & message) const;

              public:
                /**
                 * @brief Construct a new MCPToolBase object
                 * @param app Pointer to the Application instance
                 */
                explicit MCPToolBase(Application * app);

                /**
                 * @brief Virtual destructor for proper inheritance
                 */
                virtual ~MCPToolBase() = default;

                /**
                 * @brief Get the last error message for debugging
                 * @return std::string The last error message
                 */
                std::string getLastErrorMessage() const
                {
                    return m_lastErrorMessage;
                }

                /**
                 * @brief Create a structured error response with optional usage example.
                 *
                 * Generates a JSON object suitable for returning from MCP tool handlers
                 * that includes the error message and optionally a usage example and
                 * additional contextual information (e.g. available items).
                 *
                 * @param error Human-readable error description.
                 * @param usageExample Optional JSON object showing correct invocation.
                 * @param additionalInfo Optional JSON object merged into the response.
                 * @return JSON object with success=false, error, and optional extras.
                 */
                static nlohmann::json createToolError(
                    std::string const & error,
                    nlohmann::json const & usageExample = {},
                    nlohmann::json const & additionalInfo = {});
            };
        } // namespace tools
    } // namespace mcp
} // namespace gladius
