#include "PermissionPolicy.h"

namespace gladius::agent
{
    tools::PolicyDecision PermissionPolicy::evaluateTool(tools::ToolDefinition const & def,
                                                         nlohmann::json const & /*arguments*/) const
    {
        if (def.name == "capture_viewport" && !m_privacy.allowViewportImages)
        {
            return {false, "Viewport image transfer is disabled by privacy settings"};
        }
        if ((def.name == "export_raw_mesh" || def.name == "get_raw_mesh") && !m_privacy.allowRawGeometry)
        {
            return {false, "Raw geometry transfer is disabled by privacy settings"};
        }
        return {true, ""};
    }

    bool PermissionPolicy::requiresConfirmation(tools::ToolDefinition const & def,
                                                nlohmann::json const & /*arguments*/) const
    {
        if (def.policy == tools::ToolPolicy::ExternalSideEffect)
        {
            return true;
        }
        if (def.policy == tools::ToolPolicy::ReadOnly
            || def.policy == tools::ToolPolicy::ViewModification
            || def.policy == tools::ToolPolicy::DesignGraphModification)
        {
            return false;
        }

        // ToolPolicy::ModelModification
        switch (m_autonomy)
        {
        case AutonomyLevel::AskFrequently:
            return true;
        case AutonomyLevel::AskAtMajorDecisions:
            return def.name == "commit_edit" || def.name == "save_document"
                   || def.name == "save_document_as" || def.name == "export_document"
                   || def.name == "export_document_as_3mf";
        case AutonomyLevel::WorkAutonomously:
            return false;
        }
        return false;
    }
} // namespace gladius::agent
