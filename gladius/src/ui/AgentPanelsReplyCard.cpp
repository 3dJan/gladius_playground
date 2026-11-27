// The pinned reply card: the agent's last answer, shown where the user cannot miss it. A run that
// ends without a tool call ended because the model wants something from the user, and that answer
// used to be reachable only through the collapsed conversation log.

#include "AgentPanelMetrics.h"
#include "AgentPanelWidgets.h"
#include "AgentPanels.h"
#include "SignalColors.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>

#include <algorithm>
#include <string>

namespace gladius::ui
{
    namespace
    {
        /// @brief Lines shown before the card offers to expand the rest of the reply
        constexpr std::size_t kCollapsedLines = 6u;
    }

    float AgentPanels::measureReplyCard() const
    {
        if (!m_awaitingUserReply || m_running.load() || m_lastAgentMessage.empty())
        {
            return 0.f;
        }
        float const textWidth = std::max(
          1.f, m_prompt.cardOuterWidth() - ImGui::GetFontSize() * agentMetrics::cardPadding * 4.f);
        std::size_t const lines = agentWidgets::measureWrappedLines(
                                      m_lastAgentMessage.c_str(), textWidth, kCollapsedLines)
                                      .size();
        // Wrapped text, one action row, and the card's own padding.
        return static_cast<float>(lines) * ImGui::GetTextLineHeightWithSpacing()
             + ImGui::GetFrameHeight()
             + ImGui::GetFontSize() * agentMetrics::cardPadding * 2.f
             + ImGui::GetStyle().ItemSpacing.y * 2.f;
    }

    void AgentPanels::continueAfterReply()
    {
        if (m_running.load() || !m_prompt.draft().empty())
        {
            // A draft already in the field is the user's own next message; never overwrite it.
            return;
        }
        m_awaitingUserReply = false;
        startRun("Continue. Decide the open question yourself for this step, then carry on working.");
    }

    void AgentPanels::renderReplyCard()
    {
        if (!m_awaitingUserReply || m_running.load() || m_lastAgentMessage.empty())
        {
            return;
        }

        using namespace agentMetrics;
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        ImGuiStyle const & style = ImGui::GetStyle();
        float const fontSize = ImGui::GetFontSize();
        float const width = std::max(40.f, ImGui::GetContentRegionAvail().x);
        float const pad = fontSize * cardPadding;

        float const textWidth = std::max(1.f, width - pad * 2.f);
        auto lines = agentWidgets::measureWrappedLines(
          m_lastAgentMessage.c_str(), textWidth, m_replyExpanded ? 0u : kCollapsedLines);

        float const textHeight = static_cast<float>(lines.size())
                               * ImGui::GetTextLineHeightWithSpacing();
        float const cardHeight = textHeight + ImGui::GetFrameHeight() + pad * 2.f;

        ImVec2 const origin = ImGui::GetCursorScreenPos();
        ImVec2 const max(origin.x + width, origin.y + cardHeight);
        drawList.AddRectFilled(origin,
                               max,
                               ImGui::GetColorU32(ImGuiCol_PopupBg),
                               fontSize * cardRounding);
        drawList.AddRect(origin,
                         max,
                         ImGui::GetColorU32(agentWidgets::accent()),
                         fontSize * cardRounding,
                         ImDrawFlags_None,
                         1.f);

        // The prompt is the reply itself, so it gets the whole first block of the card.
        agentWidgets::drawWrappedLines(drawList,
                                       ImVec2(origin.x + pad, origin.y + pad),
                                       lines,
                                       style.Colors[ImGuiCol_Text],
                                       ImGui::GetTextLineHeightWithSpacing());

        float const actionY = origin.y + pad + textHeight + style.ItemSpacing.y * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, actionY));
        if (lines.size() >= kCollapsedLines
            && !m_replyExpanded
            && ImGui::SmallButton("Show more"))
        {
            m_replyExpanded = true;
        }
        if (m_replyExpanded && ImGui::SmallButton("Show less"))
        {
            m_replyExpanded = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLAY " Continue"))
        {
            continueAfterReply();
        }
        if (m_policy.autonomy() != agent::AutonomyLevel::WorkAutonomously)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_BOLT " Autonomous"))
            {
                m_policy.setAutonomy(agent::AutonomyLevel::WorkAutonomously);
                saveSettings();
                continueAfterReply();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                  "Switch to autonomous mode and continue: the agent decides on its own and stops "
                  "asking");
            }
        }

        // Reserve the card's box so the footer's next row starts below it.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, max.y + style.ItemSpacing.y));
    }
} // namespace gladius::ui
