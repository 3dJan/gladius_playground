#include "AgentWorkflow.h"

#include "../design/DesignGraph.h"
#include "../design/TodoStore.h"

#include <algorithm>

namespace gladius::ui
{
    namespace
    {
        constexpr std::array<char const *, kWorkflowPhaseCount> kPhaseLabels = {
          "Understand", "Decompose", "Explore", "Develop", "Validate"};

        constexpr std::array<char const *, kWorkflowPhaseCount> kPhaseHints = {
          "What the agent knows you want",
          "How the problem was broken apart",
          "Alternatives that were considered",
          "The concept being built out",
          "What was actually measured"};
    } // namespace

    char const * phaseLabel(WorkflowPhase phase) noexcept
    {
        return kPhaseLabels[indexOf(phase)];
    }

    char const * phaseHint(WorkflowPhase phase) noexcept
    {
        return kPhaseHints[indexOf(phase)];
    }

    std::size_t indexOf(WorkflowPhase phase) noexcept
    {
        return static_cast<std::size_t>(phase);
    }

    WorkflowPhase phaseAt(std::size_t index) noexcept
    {
        return static_cast<WorkflowPhase>(
          std::clamp(index, std::size_t{0}, kWorkflowPhaseCount - 1));
    }

    WorkflowPhase userPhaseOf(design::DesignPhase phase) noexcept
    {
        switch (phase)
        {
        case design::DesignPhase::ClarifyRequirements:
            return WorkflowPhase::Understand;
        case design::DesignPhase::DecomposeFunctions:
            return WorkflowPhase::Decompose;
        case design::DesignPhase::GenerateSolutionPrinciples:
        case design::DesignPhase::BuildSystemConcepts:
        case design::DesignPhase::EvaluateConcepts:
            return WorkflowPhase::Explore;
        case design::DesignPhase::InstantiateEnvelopes:
        case design::DesignPhase::DevelopComponents:
        case design::DesignPhase::IntegrateSystem:
        case design::DesignPhase::Refine:
            return WorkflowPhase::Develop;
        case design::DesignPhase::Validate:
        case design::DesignPhase::Complete:
            return WorkflowPhase::Validate;
        }
        return WorkflowPhase::Understand;
    }

    std::array<PhaseState, kWorkflowPhaseCount>
    derivePhaseStates(design::DesignGraph const & graph, WorkflowSignals const & signals)
    {
        std::array<PhaseState, kWorkflowPhaseCount> states{};
        states.fill(PhaseState::Pending);

        std::size_t const validateIdx = indexOf(WorkflowPhase::Validate);
        std::size_t const developIdx = indexOf(WorkflowPhase::Develop);
        std::size_t const activeIdx = indexOf(userPhaseOf(graph.phase()));

        if (isDesignComplete(graph))
        {
            states.fill(PhaseState::Completed);
            return states;
        }

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            states[i] = i < activeIdx ? PhaseState::Completed : PhaseState::Pending;
        }
        states[activeIdx] = PhaseState::Active;

        // Attention only ever comes from structured conditions the host actually observed.
        if (signals.agentFailed)
        {
            states[activeIdx] = PhaseState::Attention;
        }
        if (signals.planningBlocked)
        {
            states[developIdx] = PhaseState::Attention;
        }
        if (signals.deterministicWarnings > 0 && activeIdx <= validateIdx)
        {
            states[validateIdx] = PhaseState::Attention;
        }
        return states;
    }

    WorkflowPhase activePhaseOf(design::DesignGraph const & graph) noexcept
    {
        return userPhaseOf(graph.phase());
    }

    bool isDesignComplete(design::DesignGraph const & graph) noexcept
    {
        return graph.phase() == design::DesignPhase::Complete;
    }

    void countDeterministicResults(design::DesignGraph const & graph,
                                   std::size_t & passed,
                                   std::size_t & warnings) noexcept
    {
        passed = 0;
        warnings = 0;
        if (auto const active = graph.activeConfiguration())
        {
            passed = active->evaluation.deterministicPassed.size();
            warnings = active->evaluation.deterministicWarnings.size();
        }
    }

    bool hasBlockedPlanning(design::TodoStore const & todos)
    {
        return std::any_of(todos.items().begin(),
                           todos.items().end(),
                           [](design::TodoItem const & item) {
                               return item.status == design::TodoStatus::Blocked;
                           });
    }
} // namespace gladius::ui
