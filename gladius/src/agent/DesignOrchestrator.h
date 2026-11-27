#pragma once

#include "AgentEvent.h"
#include "AgentLoop.h"
#include "ContextBuilder.h"
#include "PermissionPolicy.h"
#include "UserDecision.h"
#include "providers/IAiProvider.h"
#include "trace/AgentTrace.h"
#include "../design/CadGates.h"
#include "../design/DesignGraph.h"
#include "../design/DesignTools.h"
#include "../design/ScratchDocument.h"
#include "../design/TodoStore.h"
#include "../io/CancellationToken.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gladius
{
    class Document;

    namespace compute
    {
        class IBackendRuntime;
    }
} // namespace gladius

namespace gladius::agent
{
    /// @brief Maximum redesign attempts per leaf node before escalating to failure
    inline constexpr std::size_t kMaxRedesignsPerNode = 3;

    /// @brief Actions the review/orchestrator step can decide on after a component run
    enum class OrchestratorAction
    {
        AcceptAndFreeze,
        Redesign,
        RefineInterface,
        ReviseRequirement,
        ReDecompose,
        AskUser,
        Complete
    };

    /// @brief One leaf component scheduled by the orchestrator
    struct ComponentTask
    {
        std::string nodeId;
        std::vector<std::string> dependsOn;
        std::size_t maxFlatNodes = design::kDefaultFlatNodeBudget;
    };

    /// @brief Deterministic review verdict for one component after its scoped run
    struct ComponentReviewVerdict
    {
        bool passed = false;
        OrchestratorAction action = OrchestratorAction::Redesign;
        std::string reason;
        std::size_t gatesPassed = 0;
        std::size_t gatesFailed = 0;
        design::ScratchValidationResult scratchValidation;
        std::string importedFunctionName;
    };

    /// @brief Outcome of running one leaf component (either via LLM or a test double)
    struct LeafExecutionOutcome
    {
        bool loopCompleted = false;
        std::string error;
        std::size_t modelTurns = 0;
        TokenUsage usage;
        std::size_t emptyTurns = 0;
        std::size_t truncatedTurns = 0;
        bool hasReport = false;
        design::ComponentReport report;
        design::ScratchValidationResult scratchValidation;
        std::string importedFunctionName;
    };

    /// @brief Global and per-role budgets for an orchestrated hierarchical run
    struct OrchestratorConfig
    {
        AgentLimits componentLimits{
          .maxModelTurns = 16,
          .maxToolCalls = 80,
          .maxWallTime = std::chrono::minutes(10)};
        std::size_t maxTotalModelTurns = 80;
        std::chrono::milliseconds maxTotalWallTime{std::chrono::minutes(25)};
        std::size_t maxRedesignsPerNode = kMaxRedesignsPerNode;
        std::size_t maxFlatNodesPerComponent = design::kDefaultFlatNodeBudget;
        bool composeIntoTargetDocument = true;
    };

    /// @brief Aggregate metrics emitted by `DesignOrchestrator::run`
    struct OrchestratorResult
    {
        bool completed = false;
        std::string error;
        std::size_t subAgentRuns = 0;
        std::size_t redesigns = 0;
        std::size_t totalModelTurns = 0;
        std::size_t emptyTurns = 0;
        std::size_t truncatedTurns = 0;
        TokenUsage totalUsage;
        std::map<std::string, std::size_t> perRoleTurns;
        std::map<std::string, TokenUsage> perRoleUsage;
        std::vector<std::string> executionOrder;
        std::vector<std::string> frozenNodes;
        std::vector<std::string> importedFunctions;
        std::map<std::string, ComponentReviewVerdict> verdicts;
    };

    /**
     * @brief Runs a hierarchical design sequentially across leaf components.
     * @details Dispatches each leaf only when its `dependsOn` predecessors are frozen, runs it on
     *          an isolated scratch document (or custom `LeafRunner`), evaluates deterministic CAD
     *          gates before accepting/freezing, enforces `kMaxRedesignsPerNode` with strictly-better
     *          progress checks, and composes frozen leaf functions into `main_3`.
     */
    class DesignOrchestrator
    {
      public:
        /// @brief Custom leaf executor hook (used in unit tests; production uses `AgentLoop`)
        using LeafRunner = std::function<LeafExecutionOutcome(
          ComponentTask const & task,
          std::size_t attempt,
          std::string const & feedbackForRetry,
          design::DesignGraph & graph,
          design::TodoStore & todos,
          io::CancellationToken const & cancellation)>;

        DesignOrchestrator(design::DesignGraph & graph,
                           design::TodoStore & todos,
                           Document * targetDocument,
                           compute::IBackendRuntime * runtime = nullptr);

        /// @brief Override the default `AgentLoop`-based leaf runner (for deterministic tests)
        void setLeafRunner(LeafRunner runner);

        /// @brief Provide a provider factory for production `AgentLoop` leaf runs
        void setProviderFactory(std::function<std::unique_ptr<IAiProvider>()> factory);

        /// @brief Re-queue `nodeId` and any downstream dependents after a requirement revision
        void reviseRequirementAndRequeue(std::string const & requirementId,
                                         std::string const & newDescription,
                                         std::vector<ComponentTask> const & tasks,
                                         std::vector<std::string> & pendingQueue,
                                         std::map<std::string, bool> & frozenState);

        /**
         * @brief Evaluate deterministic gates for a component outcome in fixed order:
         *        1. Component self-report present (missing report ⇒ failed)
         *        2. CadGates / ScratchValidationResult
         *        3. Interface compliance (`checkScopedInterfaces`)
         *        4. Scoped checklist entries (`TodoStore`)
         */
        [[nodiscard]] ComponentReviewVerdict evaluateComponent(
          ComponentTask const & task,
          LeafExecutionOutcome const & outcome) const;

        /// @brief Execute `tasks` sequentially respecting `dependsOn` and retry budgets
        [[nodiscard]] OrchestratorResult run(std::vector<ComponentTask> const & tasks,
                                             OrchestratorConfig const & config,
                                             io::CancellationToken const & cancellation,
                                             trace::AgentTraceRecorder * trace = nullptr);

      private:
        [[nodiscard]] LeafExecutionOutcome runLeafWithAgentLoop(
          ComponentTask const & task,
          std::size_t attempt,
          std::string const & feedbackForRetry,
          OrchestratorConfig const & config,
          io::CancellationToken const & cancellation,
          trace::AgentTraceRecorder * trace);

        design::DesignGraph & m_graph;
        design::TodoStore & m_todos;
        Document * m_targetDocument = nullptr;
        compute::IBackendRuntime * m_runtime = nullptr;
        LeafRunner m_leafRunner;
        std::function<std::unique_ptr<IAiProvider>()> m_providerFactory;
    };
} // namespace gladius::agent
