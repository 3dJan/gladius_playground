#pragma once

#include "../design/DesignTypes.h"

#include <array>
#include <cstddef>

namespace gladius::design
{
    class DesignGraph;
    class TodoStore;
} // namespace gladius::design

namespace gladius::ui
{
    /// @brief The five user-facing phases of the design workflow
    /// @details The agent's internal phase enum is finer grained; this is what the user is shown.
    ///          The mapping is deliberately lossy so the rail stays legible.
    enum class WorkflowPhase
    {
        Understand,
        Decompose,
        Explore,
        Develop,
        Validate
    };

    /// @brief Number of user-facing workflow phases
    constexpr std::size_t kWorkflowPhaseCount = 5;

    /// @brief Visual/semantic state of one workflow phase
    enum class PhaseState
    {
        Pending,
        Active,
        Completed,
        Attention
    };

    /// @brief Structured facts the host knows about the current run, never parsed from log text
    struct WorkflowSignals
    {
        bool running = false;
        bool agentFailed = false;
        bool planningBlocked = false;
        /// @brief The agent's last reply stopped on a question the user has not answered
        bool awaitingUserReply = false;
        std::size_t deterministicWarnings = 0;
    };

    [[nodiscard]] char const * phaseLabel(WorkflowPhase phase) noexcept;
    [[nodiscard]] char const * phaseHint(WorkflowPhase phase) noexcept;
    [[nodiscard]] std::size_t indexOf(WorkflowPhase phase) noexcept;
    [[nodiscard]] WorkflowPhase phaseAt(std::size_t index) noexcept;

    /// @brief Maps the agent's engineering-design phase onto the displayed workflow phase
    [[nodiscard]] WorkflowPhase userPhaseOf(design::DesignPhase phase) noexcept;

    /**
     * @brief Derives the displayed state of every phase from authoritative agent state.
     * @details Phases before the active one count as completed because the agent has moved past
     *          them; nothing here invents progress. Attention is only reported for real structured
     *          conditions: a failed run, blocked planning todos, or deterministic warnings.
     */
    [[nodiscard]] std::array<PhaseState, kWorkflowPhaseCount>
    derivePhaseStates(design::DesignGraph const & graph, WorkflowSignals const & signals);

    /// @brief Phase the agent is currently working on, per the design graph
    [[nodiscard]] WorkflowPhase activePhaseOf(design::DesignGraph const & graph) noexcept;

    /// @brief True when the whole design process has been validated and closed out
    [[nodiscard]] bool isDesignComplete(design::DesignGraph const & graph) noexcept;

    /// @brief Count of deterministic pass/warn entries for the active configuration
    void countDeterministicResults(design::DesignGraph const & graph,
                                   std::size_t & passed,
                                   std::size_t & warnings) noexcept;

    /// @brief True when any planning todo is blocked
    [[nodiscard]] bool hasBlockedPlanning(design::TodoStore const & todos);
} // namespace gladius::ui
