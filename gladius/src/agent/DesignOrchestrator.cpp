#include "DesignOrchestrator.h"

#include "../Document.h"
#include "../compute/ApplicationComputeRuntime.h"
#include "../design/InterfaceCompliance.h"
#include "../design/Renderability.h"
#include "../tools/CheckpointManager.h"
#include "../tools/ToolExecutor.h"
#include "../tools/ToolRegistry.h"
#include "../tools/TransactionManager.h"
#include "KnowledgeStore.h"
#include "SkillLoader.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace gladius::agent
{
    namespace
    {
        void accumulateUsage(TokenUsage & dst, TokenUsage const & src)
        {
            dst.inputTokens += src.inputTokens;
            dst.outputTokens += src.outputTokens;
            dst.cacheReadTokens += src.cacheReadTokens;
            dst.cacheWriteTokens += src.cacheWriteTokens;
        }
    } // namespace

    DesignOrchestrator::DesignOrchestrator(design::DesignGraph & graph,
                                           design::TodoStore & todos,
                                           Document * targetDocument,
                                           compute::IBackendRuntime * runtime)
        : m_graph(graph)
        , m_todos(todos)
        , m_targetDocument(targetDocument)
        , m_runtime(runtime)
    {
    }

    void DesignOrchestrator::setLeafRunner(LeafRunner runner)
    {
        m_leafRunner = std::move(runner);
    }

    void DesignOrchestrator::setProviderFactory(
      std::function<std::unique_ptr<IAiProvider>()> factory)
    {
        m_providerFactory = std::move(factory);
    }

    void DesignOrchestrator::reviseRequirementAndRequeue(
      std::string const & requirementId,
      std::string const & newDescription,
      std::vector<ComponentTask> const & tasks,
      std::vector<std::string> & pendingQueue,
      std::map<std::string, bool> & frozenState)
    {
        std::string category = "functional";
        bool mandatory = true;
        for (auto const & req : m_graph.requirements())
        {
            if (req.id == requirementId)
            {
                category = req.category;
                mandatory = req.mandatory;
                break;
            }
        }
        m_graph.updateRequirement(requirementId, newDescription, std::move(category), mandatory, true);

        // Unfreeze every node that directly addresses this requirement, plus its dependents.
        std::set<std::string> affected;
        for (auto const & [nodeId, node] : m_graph.nodes())
        {
            if (std::find(node.addressedRequirements.begin(),
                          node.addressedRequirements.end(),
                          requirementId)
                != node.addressedRequirements.end())
            {
                affected.insert(nodeId);
            }
        }
        bool expanded = true;
        while (expanded)
        {
            expanded = false;
            for (auto const & task : tasks)
            {
                if (affected.contains(task.nodeId))
                {
                    continue;
                }
                for (auto const & dep : task.dependsOn)
                {
                    if (affected.contains(dep))
                    {
                        affected.insert(task.nodeId);
                        expanded = true;
                        break;
                    }
                }
            }
        }
        for (auto const & nodeId : affected)
        {
            m_graph.unfreezeNode(nodeId);
            frozenState[nodeId] = false;
            if (std::find(pendingQueue.begin(), pendingQueue.end(), nodeId) == pendingQueue.end())
            {
                pendingQueue.push_back(nodeId);
            }
        }
    }

    ComponentReviewVerdict DesignOrchestrator::evaluateComponent(
      ComponentTask const & task,
      LeafExecutionOutcome const & outcome) const
    {
        ComponentReviewVerdict verdict;
        verdict.scratchValidation = outcome.scratchValidation;
        verdict.importedFunctionName = outcome.importedFunctionName;

        // 1. Missing report_component => work treated as failed, not done.
        if (!outcome.hasReport)
        {
            ++verdict.gatesFailed;
            verdict.action = OrchestratorAction::Redesign;
            verdict.reason = "Component '" + task.nodeId + "' finished without calling report_component";
            return verdict;
        }
        ++verdict.gatesPassed;

        if (!outcome.report.unsatisfied.empty())
        {
            ++verdict.gatesFailed;
            verdict.action = OrchestratorAction::Redesign;
            verdict.reason = "Component '" + task.nodeId + "' self-reported unsatisfied items: "
                             + outcome.report.unsatisfied.front();
            return verdict;
        }
        ++verdict.gatesPassed;

        // 2. CadGates / ScratchDocument validation
        if (!outcome.scratchValidation.passed)
        {
            ++verdict.gatesFailed;
            verdict.action = OrchestratorAction::Redesign;
            verdict.reason = outcome.scratchValidation.error.empty()
                               ? "Scratch validation failed"
                               : outcome.scratchValidation.error;
            return verdict;
        }
        ++verdict.gatesPassed;

        // 3. Interface compliance for the scoped node in the active configuration
        if (auto const cfg = m_graph.activeConfiguration())
        {
            auto const ifaceGate = design::checkScopedInterfaces(m_graph, *cfg, task.nodeId);
            if (!ifaceGate.passed)
            {
                ++verdict.gatesFailed;
                verdict.action = OrchestratorAction::RefineInterface;
                verdict.reason = ifaceGate.message;
                return verdict;
            }
            ++verdict.gatesPassed;
        }

        // 4. Scoped checklist entries (if any exist for this node)
        for (auto const & item : m_todos.items())
        {
            for (auto const & entry : item.checklist)
            {
                if (entry.scopeNodeId == task.nodeId && !entry.checked)
                {
                    ++verdict.gatesFailed;
                    verdict.action = OrchestratorAction::Redesign;
                    verdict.reason = "Unchecked checklist entry for node '" + task.nodeId
                                     + "': " + entry.label;
                    return verdict;
                }
            }
        }
        ++verdict.gatesPassed;

        verdict.passed = true;
        verdict.action = OrchestratorAction::AcceptAndFreeze;
        return verdict;
    }

    LeafExecutionOutcome DesignOrchestrator::runLeafWithAgentLoop(
      ComponentTask const & task,
      std::size_t attempt,
      std::string const & feedbackForRetry,
      OrchestratorConfig const & config,
      io::CancellationToken const & cancellation,
      trace::AgentTraceRecorder * trace)
    {
        LeafExecutionOutcome outcome;
        if (!m_providerFactory)
        {
            outcome.error = "No provider factory configured for DesignOrchestrator";
            return outcome;
        }
        auto provider = m_providerFactory();
        if (!provider)
        {
            outcome.error = "Provider factory returned null";
            return outcome;
        }

        auto logger = m_targetDocument != nullptr
                        ? m_targetDocument->getSharedLogger()
                        : std::make_shared<events::Logger>(events::OutputMode::Silent);
        design::ScratchComponentDocument scratch(logger, m_runtime);
        std::string initError;
        if (!scratch.initialize(initError))
        {
            outcome.error = initError;
            return outcome;
        }

        design::DesignCatalogue catalogue;
        tools::TransactionManager transactions;
        tools::CheckpointManager checkpoints;
        SkillLoader skills;
        KnowledgeStore knowledge;
        design::DesignToolContext ctx{
          m_graph,
          catalogue,
          transactions,
          checkpoints,
          scratch.document(),
          nullptr,
          nullptr,
          nullptr,
          &skills,
          &m_todos,
          &knowledge,
          /*autoCompleteDesignGraphOnFinish=*/false,
          /*maxViewportReviews=*/config.componentLimits.maxViewportReviews};

        tools::ToolRegistry registry;
        design::registerDesignTools(registry, ctx, design::DesignToolSurface::Component);

        PermissionPolicy policy;
        policy.setAutonomy(AutonomyLevel::WorkAutonomously);
        tools::ToolExecutor executor(registry,
                                     [&policy](tools::ToolDefinition const & def,
                                               nlohmann::json const & args)
                                     { return policy.evaluateTool(def, args); });

        ContextBuilder contextBuilder;
        contextBuilder.setGeometryToolSurface(true);
        contextBuilder.setRole(AgentRole::Component);

        Conversation conversation;
        std::ostringstream prompt;
        prompt << "Design component node '" << task.nodeId << "' (attempt " << (attempt + 1) << ").";
        if (auto const * node = m_graph.findNode(task.nodeId))
        {
            prompt << " Name: " << node->name << ".";
        }
        if (!feedbackForRetry.empty())
        {
            prompt << "\nPrevious attempt failed review: " << feedbackForRetry
                   << "\nFix the reported issues and call report_component when done.";
        }
        else
        {
            prompt << "\nModel the geometry in the scratch document and call report_component when done.";
        }
        conversation.appendUserText(prompt.str());

        AgentEventQueue events;
        AgentLoop loop;
        auto const runResult = coro::sync_wait(loop.run(
          conversation,
          *provider,
          registry,
          executor,
          contextBuilder,
          policy,
          events,
          config.componentLimits,
          cancellation,
          [&ctx, &task]() { return ctx.componentReports.contains(task.nodeId); },
          nullptr,
          [this, &task, &scratch, &policy]()
          {
              DesignContextSnapshot snap;
              snap.autonomy = policy.autonomy();
              snap.designPhase = design::toString(m_graph.phase());
              snap.scopeNodeId = task.nodeId;
              snap.subtreeSummary = m_graph.describeSubtree(task.nodeId);
              snap.inheritedContract = m_graph.describeInheritedContract(task.nodeId);
              if (scratch.document() != nullptr)
              {
                  auto const facts = design::collectRenderabilityFacts(scratch.document());
                  if (!facts.isRenderable())
                  {
                      snap.renderabilitySummary = facts.blockersAsText();
                  }
              }
              return snap;
          },
          trace));

        outcome.loopCompleted = runResult.completed;
        outcome.error = runResult.error;
        outcome.modelTurns = runResult.modelTurns;
        outcome.usage = runResult.usage;
        outcome.emptyTurns = runResult.emptyTurns;
        outcome.truncatedTurns = runResult.truncatedTurns;

        if (auto const it = ctx.componentReports.find(task.nodeId); it != ctx.componentReports.end())
        {
            outcome.hasReport = true;
            outcome.report = it->second;
        }

        outcome.scratchValidation = scratch.validateStandalone(task.maxFlatNodes);
        if (outcome.scratchValidation.passed && m_targetDocument != nullptr)
        {
            std::string mergeError;
            if (!scratch.mergeInto(*m_targetDocument, "", outcome.importedFunctionName, mergeError))
            {
                outcome.scratchValidation.passed = false;
                outcome.scratchValidation.error = mergeError;
            }
        }
        return outcome;
    }

    OrchestratorResult DesignOrchestrator::run(std::vector<ComponentTask> const & tasks,
                                               OrchestratorConfig const & config,
                                               io::CancellationToken const & cancellation,
                                               trace::AgentTraceRecorder * trace)
    {
        OrchestratorResult result;
        auto const startTime = std::chrono::steady_clock::now();

        std::map<std::string, ComponentTask> taskById;
        std::vector<std::string> pending;
        std::map<std::string, bool> frozen;
        std::map<std::string, std::size_t> attempts;
        std::map<std::string, std::size_t> bestGatesPassed;
        std::map<std::string, std::string> retryFeedback;

        for (auto const & task : tasks)
        {
            taskById[task.nodeId] = task;
            pending.push_back(task.nodeId);
            frozen[task.nodeId] = m_graph.isNodeFrozen(task.nodeId);
        }

        while (!pending.empty())
        {
            if (cancellation.isCancelled())
            {
                result.error = "Orchestrator cancelled";
                return result;
            }
            if (config.maxTotalModelTurns != 0 && result.totalModelTurns >= config.maxTotalModelTurns)
            {
                result.error = "Global model turn budget exceeded";
                return result;
            }
            if (config.maxTotalWallTime.count() > 0
                && std::chrono::steady_clock::now() - startTime >= config.maxTotalWallTime)
            {
                result.error = "Global wall time budget exceeded";
                return result;
            }

            // Pick the first pending task whose dependencies are all frozen.
            auto readyIt = std::find_if(pending.begin(),
                                        pending.end(),
                                        [&](std::string const & id)
                                        {
                                            auto const & deps = taskById.at(id).dependsOn;
                                            return std::all_of(deps.begin(),
                                                               deps.end(),
                                                               [&](std::string const & dep)
                                                               { return frozen[dep]; });
                                        });

            if (readyIt == pending.end())
            {
                result.error = "Dependency deadlock: no pending component has all dependencies frozen";
                return result;
            }

            std::string const nodeId = *readyIt;
            pending.erase(readyIt);

            if (frozen[nodeId])
            {
                result.frozenNodes.push_back(nodeId);
                continue;
            }

            ComponentTask const & task = taskById.at(nodeId);
            std::size_t const attempt = attempts[nodeId]++;
            ++result.subAgentRuns;
            result.executionOrder.push_back(nodeId);

            LeafExecutionOutcome const outcome =
              m_leafRunner
                ? m_leafRunner(task, attempt, retryFeedback[nodeId], m_graph, m_todos, cancellation)
                : runLeafWithAgentLoop(
                    task, attempt, retryFeedback[nodeId], config, cancellation, trace);

            result.totalModelTurns += outcome.modelTurns;
            result.emptyTurns += outcome.emptyTurns;
            result.truncatedTurns += outcome.truncatedTurns;
            accumulateUsage(result.totalUsage, outcome.usage);
            result.perRoleTurns["component"] += outcome.modelTurns;
            accumulateUsage(result.perRoleUsage["component"], outcome.usage);

            ComponentReviewVerdict verdict = evaluateComponent(task, outcome);
            result.verdicts[nodeId] = verdict;

            if (verdict.passed)
            {
                m_graph.freezeNode(nodeId);
                frozen[nodeId] = true;
                result.frozenNodes.push_back(nodeId);
                if (!verdict.importedFunctionName.empty())
                {
                    result.importedFunctions.push_back(verdict.importedFunctionName);
                }
                continue;
            }

            // Strictly-better progress guard on retries: if a redesign did not improve gate count,
            // only allow up to maxRedesignsPerNode total retries before escalating.
            if (attempt > 0 && verdict.gatesPassed <= bestGatesPassed[nodeId]
                && attempt >= config.maxRedesignsPerNode)
            {
                result.error = "Component '" + nodeId + "' failed review after "
                               + std::to_string(attempt + 1) + " attempts: " + verdict.reason;
                return result;
            }
            bestGatesPassed[nodeId] = std::max(bestGatesPassed[nodeId], verdict.gatesPassed);

            if (attempt >= config.maxRedesignsPerNode)
            {
                result.error = "Redesign budget exhausted for component '" + nodeId
                               + "': " + verdict.reason;
                return result;
            }

            ++result.redesigns;
            retryFeedback[nodeId] = verdict.reason;
            pending.push_back(nodeId);
        }

        if (config.composeIntoTargetDocument && m_targetDocument != nullptr
            && !result.importedFunctions.empty())
        {
            std::string composeError;
            if (!design::composeMain3Union(*m_targetDocument, result.importedFunctions, composeError))
            {
                result.error = "Integration composition failed: " + composeError;
                return result;
            }
        }

        result.completed = true;
        return result;
    }
} // namespace gladius::agent
