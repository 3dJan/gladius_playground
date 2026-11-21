#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace gladius::tools
{
    /// @brief Effect category of a tool for permission and transaction policies
    enum class ToolPolicy
    {
        ReadOnly,
        ViewModification,
        DesignGraphModification,
        ModelModification,
        ExternalSideEffect
    };

    /// @brief Convert a ToolPolicy enum to a stable string representation
    inline char const * toString(ToolPolicy policy) noexcept
    {
        switch (policy)
        {
        case ToolPolicy::ReadOnly:
            return "read_only";
        case ToolPolicy::ViewModification:
            return "view_modification";
        case ToolPolicy::DesignGraphModification:
            return "design_graph_modification";
        case ToolPolicy::ModelModification:
            return "model_modification";
        case ToolPolicy::ExternalSideEffect:
            return "external_side_effect";
        }
        return "unknown";
    }

    /// @brief Provider-neutral metadata and JSON schema describing a Gladius tool
    struct ToolDefinition
    {
        std::string name;
        std::string description;
        nlohmann::json inputSchema;
        ToolPolicy policy = ToolPolicy::ReadOnly;
    };
} // namespace gladius::tools
