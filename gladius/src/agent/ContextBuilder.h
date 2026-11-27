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
        /// @brief One line on whether the document actually renders, empty when it does
        std::string renderabilitySummary;
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
        /// @brief Checklist entries owned by the scoped node only (acceptance criteria for it)
        std::string assignedChecklistSummary;
        /// @brief Design-graph node this run is scoped to; empty for whole-design runs.
        /// When set, orchestrator-level state (graph summary, todos, skills, knowledge) is
        /// withheld so a component agent only ever sees its own contract.
        std::string scopeNodeId;
        /// @brief Only the scoped node and its descendants; never the whole graph
        std::string subtreeSummary;
        /// @brief Pre-formatted interface contract the scoped node must obey
        std::string inheritedContract;
    };

    /// @brief Roles of the hierarchical (divide-and-conquer) agent topology
    enum class AgentRole
    {
        Default,       ///< Monolithic design agent: owns the whole loop
        Requirements,  ///< Requirement engineer: requirements and acceptance checklist
        Decomposition, ///< Decomposer: design tree, leaf split, interface contracts
        Component,     ///< Component designer: solves one scoped node in isolation
        Review,        ///< Orchestrator review gate: evaluates one component against its contract
    };

    /// @brief Stable lowercase name of an agent role (trace/event field)
    [[nodiscard]] char const * agentRoleName(AgentRole role) noexcept;

    /// @brief Static system-prompt descriptor. Kept byte-constant per role/surface pair so the
    /// provider prompt cache survives every turn of a run.
    struct RolePrompt
    {
        AgentRole role = AgentRole::Default;
        /// @brief True when only the geometry tool surface is registered and the visual-review +
        /// finish-design ceremony is therefore part of the prompt
        bool geometryToolSurface = false;

        bool operator==(RolePrompt const & other) const noexcept
        {
            return role == other.role && geometryToolSurface == other.geometryToolSurface;
        }
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

        /// @brief Engineering-design system instructions for a role
        /// @details The Default role yields the classic monolithic prompt. All prompts are
        ///          byte-constant per RolePrompt value so the provider prompt cache survives a run.
        [[nodiscard]] static std::string defaultSystemPrompt(RolePrompt const & prompt);

        /// @brief Legacy overload: Default role, optional geometry-only tool surface
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

        /**
         * @brief Tell the builder which role of the hierarchical topology this run plays.
         * @details The role only changes the static prompt, never the live context cadence, so the
         *          prompt cache stays valid for the whole run.
         */
        void setRole(AgentRole role) noexcept
        {
            m_role = role;
        }

        /// @brief Role of this run (Default for the monolithic agent)
        [[nodiscard]] AgentRole role() const noexcept
        {
            return m_role;
        }

      private:
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
        AgentRole m_role = AgentRole::Default;
    };
} // namespace gladius::agent
