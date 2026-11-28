// Footer half of the Design Agent panel: the pinned status line, run controls and the composer
// card layout. The card's attachments live in AgentPanelsAttachments.cpp and its model pickers in
// AgentPanelsModelCombos.cpp; panel layout and the workflow tree stay in AgentPanelsUi.cpp, state
// and run control in AgentPanels.cpp.

#include "AgentPanelMetrics.h"
#include "AgentPanelText.h"
#include "AgentPanelWidgets.h"
#include "AgentPanels.h"
#include "SignalColors.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        /// @brief Banner continue button with its explanation tooltip
        bool limitContinueButton(char const * label, char const * tooltip)
        {
            if (!ImGui::Button(label))
            {
                return false;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tooltip);
            }
            return true;
        }
    } // namespace

    float AgentPanels::measureControlRowHeight() const
    {
        // Compact tool buttons are square and font-derived, so the row keeps its proportions at
        // any UI scale and matches the composer's icon buttons.
        return std::max(ImGui::GetFrameHeight(),
                        agentMetrics::compactButtonHeight * ImGui::GetFontSize());
    }

    std::vector<float> AgentPanels::measureIdleButtonWidths() const
    {
        float const size = measureControlRowHeight();
        return {size, size, size, size, size};
    }

    std::size_t AgentPanels::measureControlRows()
    {
        if (m_running.load())
        {
            // The idle tool row is not drawn while a run is active.
            return 0u;
        }
        // SameLine wrapping decides how many rows the tool row needs; measure it the same way the
        // buttons are drawn so the pinned footer can never be clipped.
        return PromptInput::countWrappedRows(measureIdleButtonWidths(),
                                             m_prompt.cardOuterWidth(),
                                             ImGui::GetStyle().ItemSpacing.x);
    }

    float AgentPanels::measureStatusLineHeight()
    {
        // The status line carries the run controls inline, so it is as tall as those buttons.
        return std::max(ImGui::GetTextLineHeightWithSpacing(),
                        measureControlRowHeight() + ImGui::GetStyle().FramePadding.y);
    }

    void AgentPanels::applyComposerMetrics()
    {
        using namespace agentMetrics;
        float const fontSize = ImGui::GetFontSize();
        PromptInput::Config config = m_prompt.config();
        // One line by default: the draft's composer is a chat field, not a large text block. The
        // field still grows to maxLines when the draft needs it.
        config.minLines = 1;
        config.maxLines = 6;
        config.minHeight = fontSize * composerMinHeight;
        config.maxHeight = fontSize * 8.f;
        config.iconButtonSize = fontSize * sendButtonSize;
        config.attachmentThumbSize = fontSize * 2.1f;
        config.placeholder = "Ask for a change...";
        config.cardPadding = std::max(4.f, fontSize * 0.30f);
        config.cardRounding = fontSize * cardRounding;
        config.minComboWidth = fontSize * 6.f;
        m_prompt.setConfig(config);
    }

    AgentPanels::FooterMetrics AgentPanels::measureFooter()
    {
        ImGuiStyle const & style = ImGui::GetStyle();
        FooterMetrics metrics;
        applyComposerMetrics();
        metrics.statusLineHeight = measureStatusLineHeight();
        metrics.controlRows = measureControlRows();
        metrics.controlRowsHeight
          = static_cast<float>(metrics.controlRows)
            * (measureControlRowHeight() + style.ItemSpacing.y);

        bool awaiting = false;
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            awaiting = m_awaitingConfirmation && !m_confirmationResolved;
        }
        if (awaiting)
        {
            float const lines = std::max(
              1.f, std::ceil(ImGui::CalcTextSize(m_pendingConfirmPrompt.c_str(), nullptr, true).x
                             / std::max(1.f,
                                        m_prompt.cardOuterWidth())));
            metrics.confirmationHeight = lines * ImGui::GetTextLineHeightWithSpacing()
                                       + ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.f;
        }
        metrics.errorLineHeight = m_lastError.empty()
                                    ? 0.f
                                    : std::max(
                                        1.f,
                                        std::ceil(ImGui::CalcTextSize(errorSummary().c_str(),
                                                                      nullptr,
                                                                      true)
                                                    .x
                                                  / std::max(
                                                      1.f,
                                                      ImGui::GetContentRegionAvail().x
                                                        - ImGui::GetFontSize() * 8.f)))
                                      * ImGui::GetTextLineHeightWithSpacing();
        metrics.replyCardHeight = measureReplyCard();
        metrics.userDecisionHeight = m_userDecisions.peek() ? userDecisionHeight() : 0.f;
        metrics.limitBannerHeight = measureLimitBanner();

        // The workflow keeps a minimum height; whatever is left belongs to the composer.
        constexpr float workflowMinHeight = 120.f;
        float const overhead = metrics.statusLineHeight + metrics.controlRowsHeight
                             + metrics.confirmationHeight + metrics.userDecisionHeight
                             + metrics.errorLineHeight + metrics.replyCardHeight
                             + metrics.limitBannerHeight;
        float const available = std::max(0.f, ImGui::GetContentRegionAvail().y - workflowMinHeight);
        metrics.composer = m_prompt.measureCard(std::max(0.f, available - overhead),
                                                m_pendingImages.size());
        metrics.totalHeight = overhead + metrics.composer.cardHeight;
        return metrics;
    }

    bool AgentPanels::iconButton(char const * icon, char const * tooltip)
    {
        float const size = std::max(measureControlRowHeight(), minTouchTargetHeight());
        bool const clicked = ImGui::Button(icon, ImVec2(size, size));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
    }

    void AgentPanels::renderStatusLine()
    {
        bool const running = m_running.load();
        bool const paused = m_runControl.isPaused();
        bool awaiting = false;
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            awaiting = m_awaitingConfirmation && !m_confirmationResolved;
        }

        // Status line: meaningful operation names, never a bare elapsed-time spinner. The run
        // controls sit inline on the right so the line is the single place to steer a running job.
        char const * statusText = nullptr;
        ImVec4 statusColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (running && m_pendingLimitKind.has_value())
        {
            statusText = "Limit reached";
            statusColor = colorWarn();
        }
        else if (running && paused)
        {
            statusText = "Paused";
            statusColor = colorWarn();
        }
        else if (running)
        {
            // The notch glyph is the activity mark; it is static, the text names the operation.
            if (!m_activeToolName.empty())
            {
                ImGui::TextColored(colorInfo(),
                                   "%s %s",
                                   ICON_FA_CIRCLE_NOTCH,
                                   agentText::humaniseName(m_activeToolName).c_str());
            }
            else
            {
                ImGui::TextColored(colorInfo(), "%s Waiting for the model", ICON_FA_CIRCLE_NOTCH);
            }
        }
        else if (awaiting)
        {
            statusText = "Action required: approve or decline below";
            statusColor = colorWarn();
        }
        else if (m_awaitingUserReply)
        {
            statusText = "Agent is waiting for your reply";
            statusColor = colorWarn();
        }
        else if (isDesignComplete(m_graph))
        {
            statusText = "Design complete";
            statusColor = colorOk();
        }
        else if (!m_conversation.empty())
        {
            statusText = "Ready";
            statusColor = colorInfo();
        }
        if (statusText != nullptr)
        {
            ImGui::TextColored(statusColor, "%s", statusText);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", m_graph.goal().empty() ? "" : m_graph.goal().c_str());
        }

        if (running)
        {
            // The tool row is empty while a run is active: the controls belong here instead.
            // Right-align them against the content edge, the way the composer aligns its buttons.
            float const buttonsWidth
              = measureControlRowHeight() * 2.f + ImGui::GetStyle().ItemSpacing.x;
            float const textMaxX = ImGui::GetCursorPosX()
                                 + std::max(0.f, ImGui::GetContentRegionAvail().x - buttonsWidth);
            if (ImGui::GetCursorPosX() > textMaxX)
            {
                ImGui::NewLine();
            }
            ImGui::SameLine(textMaxX);
            if (m_pendingLimitKind.has_value())
            {
                // Parked on a limit: the banner carries the two continue choices, so this
                // button is the plain one ("same limits for the next segment").
                if (iconButton(ICON_FA_PLAY, "Continue with a fresh budget for this segment"))
                {
                    continueAfterLimit(false);
                }
            }
            else if (paused)
            {
                if (iconButton(ICON_FA_PLAY, "Resume the current run"))
                {
                    m_runControl.resume();
                }
            }
            else if (iconButton(ICON_FA_PAUSE, "Pause the current run"))
            {
                m_runControl.requestPause();
            }
            ImGui::SameLine();
            if (iconButton(ICON_FA_STOP, "Stop the current run"))
            {
                stopRun();
            }
        }
        else
        {
            // Nothing is running, so the mode is the other thing worth pinning here.
            renderModeChip();
        }
    }

    void AgentPanels::renderModeChip()
    {
        // The mode decides whether the agent can act without asking, so it has to be visible without
        // opening Settings. The chip is the same control in both places, on the same setting.
        char const * icon = ICON_FA_USER;
        char const * label = "Ask at major decisions";
        switch (m_policy.autonomy())
        {
        case agent::AutonomyLevel::AskFrequently:
            icon = ICON_FA_QUESTION_CIRCLE;
            label = "Ask often";
            break;
        case agent::AutonomyLevel::AskAtMajorDecisions:
            icon = ICON_FA_USER_CIRCLE;
            label = "Ask at major decisions";
            break;
        case agent::AutonomyLevel::WorkAutonomously:
            icon = ICON_FA_BOLT;
            label = "Autonomous";
            break;
        }

        using namespace agentMetrics;
        // The chip sits at the right edge of the status line, opposite the status text. It reports a
        // setting rather than a problem, so it reads as a bordered label in the normal text colour:
        // an accent-coloured chip made the default mode look like a warning.
        float const fontSize = ImGui::GetFontSize();
        std::string const text = std::string(icon) + " " + label;
        ImVec2 const textSize = ImGui::CalcTextSize(text.c_str());
        ImVec2 const chipSize(textSize.x + fontSize * chipPaddingX * 2.f,
                              ImGui::GetTextLineHeight() + fontSize * chipPaddingY * 2.f);
        float const chipX = ImGui::GetCursorPosX()
                          + std::max(0.f, ImGui::GetContentRegionAvail().x - chipSize.x);
        // The pill straddles the status text instead of sitting on its baseline.
        ImGui::SameLine(chipX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - fontSize * chipPaddingY);
        ImGui::InvisibleButton("##agentModeChip", chipSize);
        bool const chipClicked = ImGui::IsItemClicked();
        bool const chipHovered = ImGui::IsItemHovered();
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        ImVec2 const chipMin = ImGui::GetItemRectMin();
        ImVec2 const chipMax = ImGui::GetItemRectMax();
        ImVec4 const chipColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        agentWidgets::drawChip(drawList, chipMin, chipMax, chipColor, chipHovered);
        ImVec4 labelText = chipColor;
        labelText.w *= chipHovered ? 1.f : chipMutedAlpha;
        drawList.AddText(ImVec2(chipMin.x + fontSize * chipPaddingX,
                                chipMin.y + fontSize * chipPaddingY),
                         ImGui::GetColorU32(labelText),
                         text.c_str());
        if (ImGui::BeginPopup("agentModePopup"))
        {
            if (ImGui::Selectable("Ask often", m_policy.autonomy() == agent::AutonomyLevel::AskFrequently))
            {
                m_policy.setAutonomy(agent::AutonomyLevel::AskFrequently);
                saveSettings();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Selectable("Ask at major decisions",
                                  m_policy.autonomy()
                                    == agent::AutonomyLevel::AskAtMajorDecisions))
            {
                m_policy.setAutonomy(agent::AutonomyLevel::AskAtMajorDecisions);
                saveSettings();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Selectable("Work autonomously",
                                  m_policy.autonomy()
                                    == agent::AutonomyLevel::WorkAutonomously))
            {
                m_policy.setAutonomy(agent::AutonomyLevel::WorkAutonomously);
                saveSettings();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (chipClicked)
        {
            ImGui::OpenPopup("agentModePopup");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Agent mode - click to change.\n%s",
                              m_policy.autonomy() == agent::AutonomyLevel::WorkAutonomously
                                ? "The agent decides on its own and never waits for you."
                                : (m_policy.autonomy() == agent::AutonomyLevel::AskFrequently
                                     ? "The agent asks before changing the model."
                                     : "The agent asks before big changes: commits, saves, exports."));
        }
    }

    std::string AgentPanels::errorSummary() const
    {
        // Show what the user can act on; the raw provider text stays in Developer diagnostics.
        return m_lastProviderError.isSet() ? m_lastProviderError.summarised()
                                           : ("Last error: " + m_lastError);
    }

    void AgentPanels::renderErrorLine()
    {
        if (m_lastError.empty())
        {
            return;
        }
        // Show what the user can act on; the raw provider text stays in Developer diagnostics.
        std::string const text = errorSummary();
        ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(colorError(), "%s", text.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", m_lastError.c_str());
        }
        if (m_lastProviderError.isSettingsFixable())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Configure providers"))
            {
                m_showSettings = true;
            }
        }
    }

    void AgentPanels::clearSession()
    {
        m_conversation.clear();
        m_activityLog.clear();
        m_lastError.clear();
        m_lastProviderError = {};
        m_pendingLimitKind.reset();
        m_pendingLimitError.clear();
        m_pendingLimitUsed = 0;
        m_lastAgentMessage.clear();
        m_awaitingUserReply = false;
        m_replyExpanded = false;
        m_streamingBuffer.clear();
        m_workflowAnimation.reset();
    }

    void AgentPanels::renderControlRow()
    {
        if (m_running.load())
        {
            // Pause/stop live on the status line while a run is active.
            return;
        }

        // Compact idle tool row: square icon buttons keep the footer short and match the composer.
        if (iconButton(ICON_FA_BROOM, "Clear the conversation and activity log"))
        {
            clearSession();
        }
        ImGui::SameLine();
        if (iconButton(ICON_FA_FILE_IMPORT,
                       "Insert a picked image as a document resource"))
        {
            insertClipboardOrFileImageIntoDocument();
        }
        ImGui::SameLine();
        // Reference images attach to the next prompt. They live on this row so the composer card
        // stays a single chat field, the way the draft has it.
        renderImageControls(measureControlRowHeight());
        ImGui::SameLine();
        if (iconButton(ICON_FA_COG, "Provider settings"))
        {
            m_showSettings = !m_showSettings;
        }
    }

    void AgentPanels::renderConfirmation()
    {
        std::lock_guard<std::mutex> lock(m_confirmMutex);
        if (!m_awaitingConfirmation || m_confirmationResolved)
        {
            return;
        }
        ImGui::TextWrapped("%s", m_pendingConfirmPrompt.c_str());
        if (ImGui::Button("Approve"))
        {
            m_confirmationApproved = true;
            m_confirmationResolved = true;
            m_confirmCv.notify_all();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline"))
        {
            m_confirmationApproved = false;
            m_confirmationResolved = true;
            m_confirmCv.notify_all();
        }
    }

    float AgentPanels::userDecisionHeight() const
    {
        auto const pending = m_userDecisions.peek();
        if (!pending.has_value())
        {
            return 0.f;
        }
        ImGuiStyle const & style = ImGui::GetStyle();
        float const lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float const width = std::max(1.f, m_prompt.cardOuterWidth());
        // Question, optional reason and the option buttons stacked on their own lines.
        float lines = std::max(
          1.f,
          std::ceil(ImGui::CalcTextSize(pending->question.c_str(), nullptr, true).x / width));
        if (!pending->reason.empty())
        {
            lines += std::max(
              1.f,
              std::ceil(ImGui::CalcTextSize(pending->reason.c_str(), nullptr, true).x / width));
        }
        lines += static_cast<float>(pending->options.size());
        // Custom answer field plus the "decide yourself" escape hatch.
        lines += 2.f;
        return lines * lineHeight + ImGui::GetFrameHeight() + style.ItemSpacing.y * 3.f;
    }

    void AgentPanels::renderUserDecision()
    {
        auto const pending = m_userDecisions.peek();
        if (!pending.has_value())
        {
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, colorInfo());
        ImGui::TextWrapped("%s", pending->question.c_str());
        ImGui::PopStyleColor();
        if (!pending->reason.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", pending->reason.c_str());
            ImGui::PopStyleColor();
        }

        auto const & options = pending->options;
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            std::string label = options[i];
            if (static_cast<int>(i) == pending->recommended)
            {
                label += " (recommended)";
            }
            if (ImGui::Button(label.c_str()))
            {
                m_userDecisions.answerOption(options[i]);
                return;
            }
        }

        ImGui::InputText("##decisionText", &m_userDecisionText);
        if (!m_userDecisionText.empty() && ImGui::Button("Send answer"))
        {
            m_userDecisions.answerText(m_userDecisionText);
            m_userDecisionText.clear();
            return;
        }
        // Never let a run park on a question the user does not want to answer.
        if (ImGui::Button("Let the agent decide"))
        {
            std::string recommendedChoice;
            if (pending->recommended >= 0
                && static_cast<std::size_t>(pending->recommended) < options.size())
            {
                recommendedChoice = options[static_cast<std::size_t>(pending->recommended)];
            }
            m_userDecisions.answerAssumed(std::move(recommendedChoice));
            m_userDecisionText.clear();
        }
    }

    float AgentPanels::leadingControlsWidth()
    {
        // The card's toolbar carries only the model pickers and the trailing expand/submit pair;
        // the image controls live on the flat tool row above the card.
        return 0.f;
    }

    void AgentPanels::renderComposerToolbar(PromptInput::ToolbarLayout const & layout, int row)
    {
        // Only the model pickers live inside the card; the leading controls of the tool row above
        // are measured through leadingControlsWidth(). PromptInput breaks a wrapped second row onto
        // its own line, so the combos never need an explicit SameLine.
        if (row + 1 == layout.rowCount && layout.drawCombos)
        {
            renderModelCombos(layout);
        }
    }

    void AgentPanels::renderComposer(PromptInput::CardMetrics const & metrics)
    {
        bool const running = m_running.load();
        PromptInput::SlotRenderer const attachments
          = [this](float) { renderPendingAttachments(); };
        PromptInput::ToolbarRenderer const toolbar
          = [this](PromptInput::ToolbarLayout const & layout, int row) {
                renderComposerToolbar(layout, row);
            };

        PromptInput::RenderResult const promptResult
          = m_prompt.renderCard(metrics, attachments, toolbar, !running);
        if (m_prompt.isFocused())
        {
            ImGuiIO const & io = ImGui::GetIO();
            if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_V, false))
            {
                attachClipboardImage();
            }
        }
        if (promptResult.sendRequested)
        {
            startRun(m_prompt.consumeDraft());
        }
        if (auto const expanded = m_prompt.renderExpandedWindow(!running); expanded)
        {
            if (expanded->sendRequested)
            {
                startRun(m_prompt.consumeDraft());
            }
        }
    }

    void AgentPanels::renderFooter()
    {
        m_prompt.setLeadingToolbarWidth(leadingControlsWidth());
        FooterMetrics const metrics = measureFooter();
        renderStatusLine();
        renderErrorLine();
        renderControlRow();
        renderConfirmation();
        renderUserDecision();
        renderReplyCard();
        renderLimitBanner();
        renderComposer(metrics.composer);
    }

    float AgentPanels::measureLimitBanner() const
    {
        if (!m_pendingLimitKind.has_value())
        {
            return 0.f;
        }
        // One wrapped text line plus the button row.
        float const lines = std::max(
          1.f,
          std::ceil(ImGui::CalcTextSize(agent::agentLimitLabel(*m_pendingLimitKind), nullptr, true)
                      .x / std::max(1.f, ImGui::GetContentRegionAvail().x)));
        return lines * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight()
             + ImGui::GetStyle().ItemSpacing.y;
    }

    void AgentPanels::renderLimitBanner()
    {
        if (!m_pendingLimitKind.has_value())
        {
            return;
        }
        // A limit is not a provider failure: say exactly which budget ran out and offer the two
        // ways to go on. The run is parked in the loop, waiting on the run control.
        ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(colorWarn(), "%s limit reached (%zu used)",
                           agent::agentLimitLabel(*m_pendingLimitKind), m_pendingLimitUsed);
        ImGui::PopTextWrapPos();
        if (!m_pendingLimitError.empty() && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", m_pendingLimitError.c_str());
        }

        if (limitContinueButton("Continue", "Finish this segment with a fresh budget"))
        {
            continueAfterLimit(false);
        }
        ImGui::SameLine();
        if (limitContinueButton("Continue unlimited",
                                "Drop every limit for the rest of this run"))
        {
            continueAfterLimit(true);
        }
    }

    void AgentPanels::continueAfterLimit(bool unlimitedFromNow)
    {
        // Viewport reviews are counted in the tool context, which the loop cannot reset: give the
        // new segment a fresh budget, or lift the cap entirely when continuing unlimited.
        if (m_toolCtx != nullptr)
        {
            m_toolCtx->viewportReviewsUsed = 0;
            if (unlimitedFromNow)
            {
                m_toolCtx->maxViewportReviews = 0;
            }
        }
        if (unlimitedFromNow)
        {
            m_limits.maxModelTurns = 0;
            m_limits.maxToolCalls = 0;
            m_limits.maxViewportReviews = 0;
            m_limits.maxWallTime = std::chrono::milliseconds(0);
        }
        m_pendingLimitKind.reset();
        m_pendingLimitError.clear();
        m_pendingLimitUsed = 0;
        m_runControl.resume(unlimitedFromNow);
    }

} // namespace gladius::ui

