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
        bool isIncremental = false;
        bool isFollowUpRequest = false;
        std::string designPhase = "ClarifyRequirements";
        std::string requirementsSummary;
        std::string requirementChangesSummary;
        std::string existingModelSummary;
        std::string designGraphSummary;
        std::string activeConfigurationSummary;
        std::string manufacturingConstraints;
        std::string relevantCatalogueSummary;
        std::string availableSkillsSummary;
        std::string validationSummary;
        std::string todoSummary;
        std::string knowledgeSummary;
    };

    /// @brief How much conversation history is worth resending to the model
    struct ContextPolicy
    {
        /// @brief Viewport images kept in the request; older ones become text stubs. One is enough:
        ///        an image costs a fixed block of prompt tokens whatever its encoded size.
        std::size_t keptImages = 1;
        /// @brief Successful tool results kept verbatim, counting back from the newest one. Failures
        ///        are always kept, because they are what the model has to react to.
        std::size_t keptToolResults = 6;
    };

    /// @brief What a build() kept or dropped, so the caller can put it in the trace
    struct ContextBuildStats
    {
        std::size_t imagesKept = 0;
        std::size_t imagesDropped = 0;
        std::size_t toolResultsKept = 0;
        std::size_t toolResultsCompacted = 0;
    };

    /// @brief Builds provider-neutral CompletionRequests while respecting privacy settings
    class ContextBuilder
    {
      public:
        /**
         * @brief Assembles the next completion request.
         *
         * The system prompt is deliberately run-constant: anything that changes between turns goes
         * into a final "current state" user message instead, because a changed system block
         * invalidates the provider's prompt cache and forces a full re-prefill.
         */
        [[nodiscard]] CompletionRequest build(Conversation const & conversation,
                                              tools::ToolRegistry const & registry,
                                              PrivacySettings const & privacy,
                                              DesignContextSnapshot const & snapshot = {},
                                              ContextBuildStats * stats = nullptr) const;

        /// @brief Default engineering-design system instructions
        [[nodiscard]] static std::string defaultSystemPrompt();

        /// @brief History budget used for image and tool-result retention
        void setContextPolicy(ContextPolicy policy) noexcept
        {
            m_policy = policy;
        }

      private:
        /// @brief Instruction-like prompt text; must stay byte-identical for the whole run
        [[nodiscard]] std::string buildStaticSystemPrompt(DesignContextSnapshot const & snapshot,
                                                          PrivacySettings const & privacy) const;

        /// @brief State that changes between turns, sent last so it extends the cached prefix
        [[nodiscard]] std::string buildLiveContext(DesignContextSnapshot const & snapshot,
                                                   PrivacySettings const & privacy) const;

        [[nodiscard]] std::vector<Message> filterHistory(Conversation const & conversation,
                                                         PrivacySettings const & privacy,
                                                         ContextBuildStats * stats) const;

        ContextPolicy m_policy;
    };
} // namespace gladius::agent
