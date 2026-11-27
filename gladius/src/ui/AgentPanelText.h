#pragma once

#include <string>

namespace gladius::ui
{
    /// @brief Text formatting shared by the Design Agent panel sections
    namespace agentText
    {
        /**
         * @brief Turns a snake_case tool id into something that reads like an operation.
         * @details "set_requirements" becomes "Set Requirements"; the transcript and the running
         *          status line both show tool names to the user, so neither should show an identifier.
         */
        [[nodiscard]] std::string humaniseName(std::string const & toolName);
    } // namespace agentText
} // namespace gladius::ui
