#pragma once

#include "../tools/ToolDefinition.h"
#include "../tools/ToolExecutor.h"

#include <nlohmann/json.hpp>

namespace gladius::agent
{
    /// @brief Controls how frequently the native agent asks the user for confirmation
    enum class AutonomyLevel
    {
        AskFrequently,
        AskAtMajorDecisions,
        WorkAutonomously
    };

    /// @brief Explicit privacy gates controlling what local data may be sent to remote AI providers
    struct PrivacySettings
    {
        bool allowConversationText = true;
        bool allowModelMetadata = true;
        bool allowValidationResults = true;
        bool allowViewportImages = true;
        bool allowReferenceImages = true;
        bool allowRawGeometry = false;
    };

    /// @brief Enforces tool-execution authority and privacy boundaries
    class PermissionPolicy
    {
      public:
        PermissionPolicy() = default;
        PermissionPolicy(AutonomyLevel autonomy, PrivacySettings privacy)
            : m_autonomy(autonomy)
            , m_privacy(privacy)
        {
        }

        void setAutonomy(AutonomyLevel autonomy) noexcept
        {
            m_autonomy = autonomy;
        }

        [[nodiscard]] AutonomyLevel autonomy() const noexcept
        {
            return m_autonomy;
        }

        void setPrivacy(PrivacySettings privacy) noexcept
        {
            m_privacy = privacy;
        }

        [[nodiscard]] PrivacySettings const & privacy() const noexcept
        {
            return m_privacy;
        }

        /// @brief Check whether a tool call is permitted outright, requires confirmation, or is blocked
        [[nodiscard]] tools::PolicyDecision
        evaluateTool(tools::ToolDefinition const & def, nlohmann::json const & arguments) const;

        /// @brief Return true if user confirmation should be requested before running this tool
        [[nodiscard]] bool
        requiresConfirmation(tools::ToolDefinition const & def, nlohmann::json const & arguments) const;

      private:
        AutonomyLevel m_autonomy = AutonomyLevel::AskAtMajorDecisions;
        PrivacySettings m_privacy;
    };
} // namespace gladius::agent
