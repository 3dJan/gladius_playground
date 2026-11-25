// Footer half of the Design Agent panel: the pinned status line, run controls and the composer
// card layout. The card's attachments live in AgentPanelsAttachments.cpp and its model pickers in
// AgentPanelsModelCombos.cpp; panel layout and the workflow tree stay in AgentPanelsUi.cpp, state
// and run control in AgentPanels.cpp.

#include "AgentPanelColors.h"
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
    std::size_t AgentPanels::measureControlRows()
    {
        // SameLine wrapping decides how many rows the control row needs; measure it the same
        // way the buttons are drawn so the pinned footer can never be clipped.
        ImGuiStyle const & style = ImGui::GetStyle();
        auto const buttonWidth = [&style](char const * label) {
            return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.f;
        };
        auto const checkWidth = [&style, &buttonWidth](char const * label) {
            return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + buttonWidth(label);
        };

        std::vector<float> widths;
        if (m_running.load())
        {
            widths.push_back(buttonWidth(m_runControl.isPaused() ? "Resume" : "Pause"));
            widths.push_back(buttonWidth("Stop"));
        }
        else
        {
            widths.push_back(buttonWidth("Clear session"));
        }
        widths.push_back(buttonWidth("Settings"));
        widths.push_back(buttonWidth("Insert image"));
        widths.push_back(checkWidth("Start fresh"));
        return PromptInput::countWrappedRows(widths,
                                            m_prompt.cardOuterWidth(),
                                            style.ItemSpacing.x);
    }

    float AgentPanels::measureStatusLineHeight()
    {
        return ImGui::GetTextLineHeightWithSpacing();
    }

    AgentPanels::FooterMetrics AgentPanels::measureFooter()
    {
        ImGuiStyle const & style = ImGui::GetStyle();
        FooterMetrics metrics;
        metrics.statusLineHeight = measureStatusLineHeight();
        metrics.controlRows = std::max<std::size_t>(1u, measureControlRows());
        metrics.controlRowsHeight
          = static_cast<float>(metrics.controlRows)
            * (std::max(ImGui::GetFrameHeight(), ImGui::GetTextLineHeightWithSpacing())
               + style.ItemSpacing.y);

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

    void AgentPanels::renderStatusLine()
    {
        bool const running = m_running.load();
        bool const paused = m_runControl.isPaused();
        bool awaiting = false;
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            awaiting = m_awaitingConfirmation && !m_confirmationResolved;
        }

        // Status line: meaningful operation names, never a bare elapsed-time spinner.
        if (running && paused)
        {
            ImGui::TextColored(colorWarn(), "Paused - press Resume to continue");
        }
        else if (running)
        {
            char const * spinner[] = {"|", "/", "-", "\\"};
            int const frame = static_cast<int>(ImGui::GetTime() * 6.0) % 4;
            if (!m_activeToolName.empty())
            {
                ImGui::TextColored(colorInfo(),
                                   "%s Running %s",
                                   spinner[frame],
                                   humaniseToolName(m_activeToolName).c_str());
            }
            else
            {
                ImGui::TextColored(colorInfo(), "%s Waiting for the model", spinner[frame]);
            }
        }
        else if (awaiting)
        {
            ImGui::TextColored(colorWarn(), "Action required: approve or decline below");
        }
        else if (isDesignComplete(m_graph))
        {
            ImGui::TextColored(colorOk(), "Design complete");
        }
        else if (!m_conversation.empty())
        {
            ImGui::TextColored(colorInfo(), "Waiting for your instruction");
        }
        else
        {
            ImGui::TextDisabled("Describe what you want to design");
        }
    }

    void AgentPanels::renderControlRow()
    {
        bool const running = m_running.load();
        if (running)
        {
            if (m_runControl.isPaused())
            {
                if (ImGui::Button("Resume"))
                {
                    m_runControl.resume();
                }
            }
            else if (ImGui::Button("Pause"))
            {
                m_runControl.requestPause();
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
            {
                stopRun();
            }
        }
        else if (ImGui::Button("Clear session"))
        {
            m_conversation.clear();
            m_activityLog.clear();
            m_lastError.clear();
            m_streamingBuffer.clear();
            m_workflowAnimation.reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Settings"))
        {
            m_showSettings = !m_showSettings;
        }
        ImGui::SameLine();
        if (ImGui::Button("Insert image"))
        {
            insertClipboardOrFileImageIntoDocument();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Insert the clipboard or a picked image as a document resource");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Start fresh", &m_startFresh);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
              "Clear current model & history on next prompt (auto-unchecks after first prompt)");
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
        ImGuiStyle const & style = ImGui::GetStyle();
        float const size = m_prompt.config().iconButtonSize;
        return size * 2.f + style.ItemSpacing.x;
    }

    void AgentPanels::renderComposerToolbar(PromptInput::ToolbarLayout const & layout, int row)
    {
        if (row == 0 && layout.drawLeading)
        {
            renderImageControls(m_prompt.config().iconButtonSize);
        }
        if (row + 1 == layout.rowCount && layout.drawCombos)
        {
            if (row == 0)
            {
                ImGui::SameLine();
            }
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

