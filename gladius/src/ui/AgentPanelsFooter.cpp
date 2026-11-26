// Footer half of the Design Agent panel: the pinned status line, run controls and the composer
// card layout. The card's attachments live in AgentPanelsAttachments.cpp and its model pickers in
// AgentPanelsModelCombos.cpp; panel layout and the workflow tree stay in AgentPanelsUi.cpp, state
// and run control in AgentPanels.cpp.

#include "AgentPanelColors.h"
#include "AgentPanelMetrics.h"
#include "AgentPanelWidgets.h"
#include "AgentPanels.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::ui
{
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
        return {size, size, size, size, size, size};
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

        // The workflow keeps a minimum height; whatever is left belongs to the composer.
        constexpr float workflowMinHeight = 120.f;
        float const overhead = metrics.statusLineHeight + metrics.controlRowsHeight
                             + metrics.confirmationHeight;
        float const available = std::max(0.f, ImGui::GetContentRegionAvail().y - workflowMinHeight);
        metrics.composer = m_prompt.measureCard(std::max(0.f, available - overhead),
                                                m_pendingImages.size());
        metrics.totalHeight = overhead + metrics.composer.cardHeight;
        return metrics;
    }

    bool AgentPanels::iconButton(char const * icon, char const * tooltip)
    {
        float const size = measureControlRowHeight();
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
        if (running && paused)
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
                                   humaniseToolName(m_activeToolName).c_str());
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
            if (paused)
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
    }

    void AgentPanels::clearSession()
    {
        m_conversation.clear();
        m_activityLog.clear();
        m_lastError.clear();
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
        if (iconButton(ICON_FA_SYNC_ALT,
                       "Clear current model & history on next prompt (auto-unchecks after first "
                       "prompt)"))
        {
            m_startFresh = !m_startFresh;
        }
        if (m_startFresh)
        {
            // A filled corner dot marks the toggle as armed without taking horizontal space.
            ImDrawList & drawList = *ImGui::GetWindowDrawList();
            ImVec2 const max = ImGui::GetItemRectMax();
            float const fontSize = ImGui::GetFontSize();
            drawList.AddCircleFilled(ImVec2(max.x - fontSize * 0.30f, max.y - fontSize * 0.30f),
                                     fontSize * 0.13f,
                                     ImGui::GetColorU32(agentWidgets::accent()));
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
        renderControlRow();
        renderConfirmation();
        renderComposer(metrics.composer);
    }

} // namespace gladius::ui

