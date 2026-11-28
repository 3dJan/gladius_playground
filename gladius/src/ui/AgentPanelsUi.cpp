// UI half of the Design Agent: the single consolidated panel, its persistent footer, and the
// collapsible diagnostics. State, run control, and persistence stay in AgentPanels.cpp.

#include "AgentPanelDocking.h"
#include "AgentPanels.h"
#include "SignalColors.h"

#include "AgentPanelMetrics.h"
#include "AgentPanelWidgets.h"
#include "AgentWorkflow.h"
#include "AgentWorkflowAnimation.h"
#include "AgentWorkflowView.h"
#include "ClipboardImageHelper.h"
#include "WorkflowArtifacts.h"

#include "../Document.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <tinyfiledialogs/tinyfiledialogs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace gladius::ui
{
    WorkflowSignals AgentPanels::workflowSignals() const
    {
        WorkflowSignals signals;
        signals.running = m_running.load();
        signals.agentFailed = !m_lastError.empty();
        signals.planningBlocked = hasBlockedPlanning(m_todos);
        signals.awaitingUserReply = m_awaitingUserReply && !m_running.load();
        std::size_t passed = 0;
        countDeterministicResults(m_graph, passed, signals.deterministicWarnings);
        return signals;
    }

    std::string AgentPanels::phaseSummary(std::size_t index) const
    {
        if (index == indexOf(WorkflowPhase::Understand))
        {
            std::size_t const hard = static_cast<std::size_t>(
              std::count_if(m_graph.requirements().begin(),
                            m_graph.requirements().end(),
                            [](design::Requirement const & r) { return r.mandatory; }));
            std::size_t const soft = m_graph.requirements().size() - hard;
            if (m_graph.requirements().empty())
            {
                return "Nothing captured yet";
            }
            return std::to_string(hard) + (hard == 1 ? " constraint, " : " constraints, ")
                 + std::to_string(soft) + (soft == 1 ? " preference" : " preferences");
        }
        if (index == indexOf(WorkflowPhase::Decompose))
        {
            if (m_graph.nodes().empty())
            {
                return "Not decomposed yet";
            }
            return std::to_string(m_graph.nodes().size()) + " elements";
        }
        if (index == indexOf(WorkflowPhase::Explore))
        {
            std::size_t ready = 0;
            std::size_t rejected = 0;
            for (auto const & [id, variant] : m_graph.variants())
            {
                (void) id;
                (variant.status == design::DesignStatus::Rejected ? rejected : ready)++;
            }
            if (m_graph.variants().empty())
            {
                return "No alternatives generated yet";
            }
            std::string text = std::to_string(ready) + " alternatives ready";
            if (rejected > 0)
            {
                text += ", " + std::to_string(rejected) + " rejected";
            }
            return text;
        }
        if (index == indexOf(WorkflowPhase::Develop))
        {
            std::size_t const done = static_cast<std::size_t>(
              std::count_if(m_todos.items().begin(),
                            m_todos.items().end(),
                            [](design::TodoItem const & t) {
                                return t.status == design::TodoStatus::Done;
                            }));
            if (m_todos.items().empty())
            {
                return "No planning tasks yet";
            }
            return std::to_string(done) + " of " + std::to_string(m_todos.items().size())
                   + " tasks complete";
        }

        std::size_t passed = 0;
        std::size_t warnings = 0;
        countDeterministicResults(m_graph, passed, warnings);
        if (isDesignComplete(m_graph))
        {
            return "Design finished - " + std::to_string(passed) + " checks measured";
        }
        if (passed + warnings == 0)
        {
            return "Nothing measured yet";
        }
        return std::to_string(passed) + " measured ok, " + std::to_string(warnings) + " warnings";
    }

    void AgentPanels::render()
    {
        if (!m_visible)
        {
            return;
        }
        drainEvents();
        renderWorkflowPanel();
        if (m_showSettings)
        {
            renderProviderSettingsPanel();
        }
    }

    void AgentPanels::renderWorkflowPanel()
    {
        // The panel draws its own header, so ImGui's title bar is replaced rather than styled.
        // No position or size is requested: while docked (the default) the dockspace owns both, and a
        // SetNextWindowPos would undock the panel every frame.
        // Full screen on a phone means rows would otherwise run into the physical screen edge.
        bool const compact = SurfaceProfileStore::instance().isCompact();
        if (compact)
        {
            float const edgeMargin =
              ImGui::GetIO().DisplaySize.x * (1.f - agentMetrics::panelCompactContentMaxWidthRatio) *
              0.5f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(edgeMargin, ImGui::GetStyle().WindowPadding.y));
        }

        if (!ImGui::Begin(kAgentPanelWindowName, &m_visible, ImGuiWindowFlags_NoTitleBar))
        {
            if (compact)
            {
                ImGui::PopStyleVar();
            }
            ImGui::End();
            return;
        }

        auto const headerCommands = m_header.render(m_graph.goal());
        if (headerCommands.goalCommitRequested)
        {
            m_graph.setGoal(headerCommands.goalDraft);
        }
        if (headerCommands.settingsRequested)
        {
            m_showSettings = !m_showSettings;
        }
        if (headerCommands.clearSessionRequested)
        {
            clearSession();
        }
        if (headerCommands.hideRequested)
        {
            m_visible = false;
        }

        WorkflowSignals const signals = workflowSignals();
        auto const states = derivePhaseStates(m_graph, signals);
        std::size_t const activeIndex = indexOf(activePhaseOf(m_graph));

        float const nowSeconds = static_cast<float>(ImGui::GetTime());
        if (m_lastActivePhaseIndex != activeIndex)
        {
            // Reaching a new phase expands it: the user always sees what the agent is doing now.
            m_phaseExpanded[activeIndex] = true;
            if (m_lastActivePhaseIndex < kWorkflowPhaseCount)
            {
                m_phaseExpanded[m_lastActivePhaseIndex] = false;
            }
            m_lastActivePhaseIndex = activeIndex;
        }
        m_workflowAnimation.update(activeIndex, states, nowSeconds);

        std::array<std::string, kWorkflowPhaseCount> summaries;
        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            summaries[i] = phaseSummary(i);
        }

        m_artifacts.activeToolName = m_activeToolName;
        // While a run holds the graph, UI requirement edits go to the staging queue; the rows still
        // draw the merged view, so nothing the user typed disappears.
        m_artifacts.bindRequirementEdits(m_running.load() ? &m_requirementEdits : nullptr);
        std::array<AgentWorkflowView::ArtifactRenderer, kWorkflowPhaseCount> const renderers = {
          [this]() { m_artifacts.renderUnderstand(m_workflowAnimation); },
          [this]() { m_artifacts.renderDecompose(m_workflowAnimation); },
          [this]() { m_artifacts.renderExplore(m_workflowAnimation); },
          [this]() { m_artifacts.renderDevelop(m_workflowAnimation); },
          [this]() { m_artifacts.renderValidate(m_workflowAnimation); }};

        float const footerHeight = measureFooter().totalHeight;
        ImGui::BeginChild("WorkflowScroll", ImVec2(0.f, -footerHeight), true);
        m_workflowView.render(states, activeIndex, summaries, m_phaseExpanded, renderers,
                              m_workflowAnimation);
        ImGui::Spacing();
        ImGui::Separator();
        renderConversationSection();
        renderDiagnosticsSection();
        ImGui::EndChild();

        renderFooter();
        ImGui::End();
        if (compact)
        {
            ImGui::PopStyleVar();
        }
    }

    void AgentPanels::renderConversationSection()
    {
        // Flat toggle row instead of a CollapsingHeader: same visual family as the section rows.
        // The row is always there, even with an empty transcript, so the section can be discovered
        // before the first turn exists.
        if (!agentWidgets::toggleRow("##conversationSection",
                                     ICON_FA_COMMENTS,
                                     "Conversation",
                                     m_conversationExpanded))
        {
            return;
        }
        std::vector<AgentConversationView::Turn> const turns
          = AgentConversationView::buildTurns(m_conversation, m_showDeveloperTrace);
        if (turns.empty())
        {
            m_conversationView.renderEmptyHint();
            return;
        }
        m_conversationView.render(turns, m_transcriptExpanded);
    }

    void AgentPanels::renderDiagnosticsSection()
    {
        std::string const activityLabel = "Activity log (" + std::to_string(m_activityLog.size())
                                        + " steps)";
        if (!m_activityLog.empty()
            && agentWidgets::toggleRow("##activityLogSection",
                                       ICON_FA_HISTORY,
                                       activityLabel.c_str(),
                                       m_activityLogExpanded))
        {
            for (std::size_t idx = 0; idx < m_activityLog.size(); ++idx)
            {
                auto const & entry = m_activityLog[idx];
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TextColored(entry.success ? colorInfo() : colorError(),
                                   "%s",
                                   entry.label.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy"))
                {
                    std::string const text = entry.label + "\n" + entry.detail;
                    ImGui::SetClipboardText(text.c_str());
                }
                if (m_showDeveloperTrace && !entry.detail.empty())
                {
                    std::string detail = entry.detail;
                    ImGui::PushID("detail");
                    ImGui::InputTextMultiline("##detail",
                                              detail.data(),
                                              detail.size() + 1,
                                              ImVec2(-1.f, ImGui::GetTextLineHeight() * 4.f),
                                              ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }

        if (agentWidgets::toggleRow("##diagnosticsSection",
                                    ICON_FA_WRENCH,
                                    "Developer diagnostics",
                                    m_diagnosticsExpanded))
        {
            ImGui::Checkbox("Developer trace", &m_showDeveloperTrace);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy trace"))
            {
                ImGui::SetClipboardText(buildDeveloperTrace().c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear log"))
            {
                m_activityLog.clear();
            }

            ImGui::Text("Model: %s",
                        m_primarySelection.profileId.empty()
                          ? "not selected"
                          : (m_primarySelection.profileId + " / " + m_primarySelection.model).c_str());
            ImGui::Text("Fallbacks: %zu | Phase: %s",
                        m_fallbackSelections.size(),
                        design::toString(m_graph.phase()));
            if (m_modelFetchRunning.load())
            {
                ImGui::TextDisabled("Querying provider models...");
            }
            if (!m_credentialWarning.empty())
            {
                ImGui::TextColored(colorWarn(), "%s", m_credentialWarning.c_str());
            }
            if (!m_lastError.empty())
            {
                ImGui::TextColored(colorError(), "Last error: %s", m_lastError.c_str());
            }
            if (!m_streamingBuffer.empty())
            {
                ImGui::TextDisabled("Streaming: %s", m_streamingBuffer.c_str());
            }

            if (m_showDeveloperTrace)
            {
                for (auto const & msg : m_conversation.messages())
                {
                    for (auto const & part : msg.parts)
                    {
                        if (auto const * tc = std::get_if<agent::ToolCall>(&part))
                        {
                            ImGui::TextWrapped("[ToolCall] %s %s",
                                               tc->name.c_str(),
                                               tc->arguments.dump().c_str());
                        }
                        else if (auto const * tr = std::get_if<agent::ToolResult>(&part))
                        {
                            ImGui::TextWrapped("[ToolResult] %s", tr->value.dump().c_str());
                        }
                    }
                }
            }
        }
    }

    std::string AgentPanels::buildDeveloperTrace() const
    {
        std::string trace;
        for (auto const & entry : m_activityLog)
        {
            trace += entry.label + "\n";
            if (!entry.detail.empty())
            {
                trace += entry.detail + "\n";
            }
        }
        for (auto const & msg : m_conversation.messages())
        {
            char const * role = msg.role == agent::MessageRole::Assistant
                                  ? "Agent"
                                  : (msg.role == agent::MessageRole::Tool ? "Tool" : "User");
            for (auto const & part : msg.parts)
            {
                if (auto const * t = std::get_if<agent::TextContent>(&part))
                {
                    trace += std::string("[") + role + "] " + t->text + "\n";
                }
                else if (auto const * tc = std::get_if<agent::ToolCall>(&part))
                {
                    trace += "[ToolCall] " + tc->name + ": " + tc->arguments.dump() + "\n";
                }
                else if (auto const * tr = std::get_if<agent::ToolResult>(&part))
                {
                    trace += "[ToolResult] " + tr->callId + ": " + tr->value.dump() + "\n";
                }
            }
        }
        if (!m_lastError.empty())
        {
            trace += "[Error] " + m_lastError + "\n";
        }
        return trace;
    }
} // namespace gladius::ui
