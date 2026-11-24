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
    /// @details History is append-only: a message that has been sent once is sent again unchanged.
    ///          Sliding-window retention (keep the newest N images, compact everything older) rewrites
    ///          the oldest messages on every turn, which breaks the endpoint's prompt cache right after
    ///          the tool block and makes it re-prefill the whole conversation. Old content is dropped
    ///          in whole messages instead, in chunks, so the sent prefix stays byte-identical between
    ///          prunes and the cache only breaks at a chunk boundary.
    struct ContextPolicy
    {
        /// @brief Conversation messages sent before the oldest part starts being dropped; 0 is unlimited
        std::size_t maxHistoryMessages = 96;
        /// @brief Messages dropped at once once the budget is exceeded
        std::size_t pruneChunkMessages = 32;
    };

    /// @brief What a build() kept or dropped, so the caller can put it in the trace
    struct ContextBuildStats
    {
        std::size_t imagesKept = 0;
        /// @brief Conversation messages actually sent
        std::size_t messagesSent = 0;
        /// @brief Oldest messages dropped because the history budget was exceeded
        std::size_t messagesPruned = 0;
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
        /// @param geometryToolSurface true when the host only registered the geometry tool surface,
        ///        in which case the planning, variant, and ceremony instructions are left out
        [[nodiscard]] static std::string defaultSystemPrompt(bool geometryToolSurface = false);

        /// @brief History budget used for image and tool-result retention
        void setContextPolicy(ContextPolicy policy) noexcept
        {
            m_policy = policy;
        }

        /**
         * @brief Tell the builder that only the geometry tool surface is registered.
         * @details Prompt text that describes unregistered tools is pure cost: it is re-prefilled on
         *          every turn and invites the model to plan steps it cannot perform.
         */
        void setGeometryToolSurface(bool geometryOnly) noexcept
        {
            m_geometryToolSurface = geometryOnly;
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
        bool m_geometryToolSurface = false;
    };
} // namespace gladius::agent
