#pragma once

#include "Conversation.h"
#include "PermissionPolicy.h"
#include "providers/IAiProvider.h"
#include "../tools/ToolRegistry.h"

#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Structured engineering context injected into each model turn
    struct DesignContextSnapshot
    {
        AutonomyLevel autonomy = AutonomyLevel::AskAtMajorDecisions;
        std::string designPhase = "ClarifyRequirements";
        std::string requirementsSummary;
        std::string designGraphSummary;
        std::string activeConfigurationSummary;
        std::string manufacturingConstraints;
        std::string relevantCatalogueSummary;
        std::string availableSkillsSummary;
        std::string validationSummary;
    };

    /// @brief Builds provider-neutral CompletionRequests while respecting privacy settings
    class ContextBuilder
    {
      public:
        [[nodiscard]] CompletionRequest build(Conversation const & conversation,
                                              tools::ToolRegistry const & registry,
                                              PrivacySettings const & privacy,
                                              DesignContextSnapshot const & snapshot = {}) const;

        /// @brief Default engineering-design system instructions
        [[nodiscard]] static std::string defaultSystemPrompt();
    };
} // namespace gladius::agent
