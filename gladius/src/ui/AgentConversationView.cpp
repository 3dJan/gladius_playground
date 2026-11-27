// The transcript: what the agent said, what the user asked, and (on request) the tool chatter in
// between. It used to be one flat list of "[Agent] ..." lines at the window margin. This version
// keeps the panel's visual vocabulary: role chips on the rail column, turn text on the phase title
// column, and the same hover band the sections use.

#include "AgentConversationView.h"

#include "AgentPanelColors.h"
#include "AgentPanelMetrics.h"
#include "AgentPanelText.h"
#include "AgentPanelWidgets.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        /// @brief Turns shown while the transcript is collapsed
        constexpr std::size_t kCollapsedTurns = 4u;
        /// @brief Wrapped body lines of one collapsed turn
        constexpr std::size_t kCollapsedBodyLines = 4u;
        /// @brief Wrapped body lines of one collapsed tool trace
        constexpr std::size_t kCollapsedToolLines = 2u;
        /// @brief Characters of a tool result kept before it is cut, so the panel stays readable
        constexpr std::size_t kToolBodyChars = 400u;

        char const * turnIcon(AgentConversationView::TurnKind kind)
        {
            switch (kind)
            {
            case AgentConversationView::TurnKind::Agent:
                return ICON_FA_BRAIN;
            case AgentConversationView::TurnKind::User:
                return ICON_FA_USER;
            case AgentConversationView::TurnKind::System:
                return ICON_FA_COMMENT_DOTS;
            case AgentConversationView::TurnKind::Tool:
                return ICON_FA_WRENCH;
            }
            return ICON_FA_COMMENT_DOTS;
        }

        ImVec4 roleColor(AgentConversationView::TurnKind kind)
        {
            switch (kind)
            {
            case AgentConversationView::TurnKind::Agent:
                return agentWidgets::accent();
            case AgentConversationView::TurnKind::User:
                return colorInfo();
            case AgentConversationView::TurnKind::System:
            case AgentConversationView::TurnKind::Tool:
                return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            }
            return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        }

        ImVec4 bodyColor(AgentConversationView::TurnKind kind)
        {
            if (kind == AgentConversationView::TurnKind::System
                || kind == AgentConversationView::TurnKind::Tool)
            {
                ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                color.w *= agentMetrics::mutedBodyAlpha;
                return color;
            }
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }

        /// @brief One line of JSON, collapsed so a tool result cannot sprawl over the panel
        std::string summariseJsonValue(nlohmann::json const & value)
        {
            std::string text = value.is_string() ? value.get<std::string>() : value.dump();
            std::replace(text.begin(), text.end(), '\n', ' ');
            std::replace(text.begin(), text.end(), '\r', ' ');
            if (text.size() > kToolBodyChars)
            {
                text.resize(kToolBodyChars);
                text += "...";
            }
            return text;
        }

        /// @brief Joins the text parts of a message, one paragraph per part
        std::string joinTextParts(agent::Message const & message)
        {
            std::string text;
            for (auto const & part : message.parts)
            {
                if (auto const * content = std::get_if<agent::TextContent>(&part);
                    content != nullptr)
                {
                    if (!text.empty())
                    {
                        text += "\n";
                    }
                    text += content->text;
                }
            }
            return text;
        }

        bool hasImagePart(agent::Message const & message)
        {
            return std::any_of(message.parts.begin(),
                               message.parts.end(),
                               [](agent::Content const & part) {
                                   return std::holds_alternative<agent::ImageContent>(part);
                               });
        }

        /// @brief Human readable names of the tools an assistant message asked for
        std::string toolCallSummary(agent::Message const & message)
        {
            std::string text;
            for (auto const & part : message.parts)
            {
                if (auto const * call = std::get_if<agent::ToolCall>(&part); call != nullptr)
                {
                    if (!text.empty())
                    {
                        text += ", ";
                    }
                    text += agentText::humaniseName(call->name);
                }
            }
            return text;
        }

        /// @brief Soft disc with the role glyph centred in it, on the rail column
        void drawRoleChip(ImDrawList & drawList,
                          ImVec2 center,
                          float radius,
                          ImVec4 color,
                          char const * icon)
        {
            ImVec4 fill = color;
            fill.w *= 0.22f;
            drawList.AddCircleFilled(center, radius, ImGui::GetColorU32(fill), 28);
            drawList.AddCircle(center, radius, ImGui::GetColorU32(color), 28, 1.f);
            ImVec2 const iconSize = ImGui::CalcTextSize(icon);
            drawList.AddText(ImVec2(center.x - iconSize.x * 0.5f, center.y - iconSize.y * 0.5f),
                             ImGui::GetColorU32(color),
                             icon);
        }
    } // namespace

    std::vector<AgentConversationView::Turn>
      AgentConversationView::buildTurns(agent::Conversation const & conversation,
                                        bool includeToolTrace)
    {
        std::vector<Turn> turns;
        for (auto const & message : conversation.messages())
        {
            switch (message.role)
            {
            case agent::MessageRole::Assistant:
            {
                Turn turn;
                turn.kind = TurnKind::Agent;
                turn.title = "Agent";
                turn.body = joinTextParts(message);
                turn.hasImage = hasImagePart(message);
                if (turn.body.empty())
                {
                    std::string const calls = toolCallSummary(message);
                    turn.body = calls.empty() ? std::string{} : "Called " + calls;
                }
                if (!turn.body.empty() || turn.hasImage)
                {
                    turns.push_back(std::move(turn));
                }
                break;
            }
            case agent::MessageRole::User:
            {
                Turn turn;
                turn.kind = TurnKind::User;
                turn.title = "You";
                turn.body = joinTextParts(message);
                turn.hasImage = hasImagePart(message);
                if (!turn.body.empty() || turn.hasImage)
                {
                    turns.push_back(std::move(turn));
                }
                break;
            }
            case agent::MessageRole::System:
            {
                if (!includeToolTrace)
                {
                    break;
                }
                Turn turn;
                turn.kind = TurnKind::System;
                turn.title = "System";
                turn.body = joinTextParts(message);
                turn.hasImage = hasImagePart(message);
                if (!turn.body.empty() || turn.hasImage)
                {
                    turns.push_back(std::move(turn));
                }
                break;
            }
            case agent::MessageRole::Tool:
            {
                if (!includeToolTrace)
                {
                    break;
                }
                for (auto const & part : message.parts)
                {
                    if (auto const * result = std::get_if<agent::ToolResult>(&part);
                        result != nullptr)
                    {
                        Turn turn;
                        turn.kind = TurnKind::Tool;
                        turn.title = "Tool result";
                        turn.body = summariseJsonValue(result->value);
                        turns.push_back(std::move(turn));
                    }
                }
                break;
            }
            }
        }
        return turns;
    }

    void AgentConversationView::measureColumns()
    {
        using namespace agentMetrics;
        float const fontSize = ImGui::GetFontSize();
        float const rowStartX = ImGui::GetCursorScreenPos().x;
        float const availWidth = std::max(64.f, ImGui::GetContentRegionAvail().x);
        // One column vocabulary with the rail above: the chips share the rail line, the turn text
        // shares the phase title line. Screen coordinates end to end, as in the other sections.
        m_chipX = rowStartX + fontSize * railOffset;
        m_textX = m_chipX + fontSize * (railTextGap + chevronGutter);
        m_rightX = rowStartX + availWidth;
    }

    void AgentConversationView::renderEmptyHint()
    {
        m_lastTurns.clear();
        measureColumns();
        char const * hint = "Nothing said yet. Ask the agent what the part should do.";
        agentWidgets::ReservedRow const row
          = agentWidgets::reserveRow(ImGui::GetTextLineHeight(), "##conversationEmpty");
        agentWidgets::drawClippedText(*ImGui::GetWindowDrawList(),
                                      ImVec2(m_textX, row.origin.y),
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                      hint,
                                      m_rightX - m_textX);
        agentWidgets::endReservedRow(row);
    }

    void AgentConversationView::render(std::vector<Turn> const & turns, bool & expanded)
    {
        m_lastTurns.clear();
        if (turns.empty())
        {
            return;
        }
        measureColumns();

        std::size_t const first = (!expanded && turns.size() > kCollapsedTurns)
                                    ? turns.size() - kCollapsedTurns
                                    : 0u;
        if (first > 0u
            && toggleRow(ICON_FA_ARROW_ALT_CIRCLE_DOWN,
                         "Show " + std::to_string(first) + " earlier turns"))
        {
            expanded = true;
        }

        float const wrapWidth = std::max(48.f, m_rightX - m_textX);
        float const ellipsisWidth = ImGui::CalcTextSize("...").x;
        for (std::size_t i = first; i < turns.size(); ++i)
        {
            // A turn of the same speaker as the one before it needs no second role line, but the
            // first turn shown always gets one: it is what names the speaker.
            bool const speakerChanged = i > first && turns[i - 1].kind != turns[i].kind;
            // Every turn row submits an invisible button, so each needs its own ID.
            ImGui::PushID(static_cast<int>(i));
            bool const clicked = renderTurn(turns[i],
                                            expanded,
                                            speakerChanged,
                                            i == first || speakerChanged,
                                            wrapWidth,
                                            ellipsisWidth);
            ImGui::PopID();
            // Clicking a cut-off turn is the natural way to ask for the rest of it.
            if (clicked && !expanded)
            {
                expanded = true;
            }
        }
        if (expanded && turns.size() > kCollapsedTurns
            && toggleRow(ICON_FA_ARROW_ALT_CIRCLE_UP, "Show only the latest turns"))
        {
            expanded = false;
        }
    }

    bool AgentConversationView::toggleRow(char const * icon, std::string const & label)
    {
        ImGui::PushID(label.c_str());
        agentWidgets::ReservedRow const row
          = agentWidgets::reserveRow(ImGui::GetTextLineHeight(), "##turnsToggle");
        bool const clicked = ImGui::IsItemClicked();
        ImGui::PopID();
        agentWidgets::drawRowBand(row, false, row.hovered);

        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        ImVec4 const color = agentWidgets::accent();
        ImVec2 const pos(row.origin.x + ImGui::GetFontSize() * 0.25f, row.origin.y);
        drawList.AddText(pos, ImGui::GetColorU32(color), icon);
        drawList.AddText(ImVec2(pos.x + ImGui::CalcTextSize(icon).x
                                    + ImGui::GetStyle().ItemInnerSpacing.x,
                                pos.y),
                         ImGui::GetColorU32(color),
                         label.c_str());
        agentWidgets::endReservedRow(row);
        return clicked;
    }

    bool AgentConversationView::renderTurn(Turn const & turn,
                                           bool expanded,
                                           bool speakerChanged,
                                           bool showRole,
                                           float wrapWidth,
                                           float ellipsisWidth)
    {
        using namespace agentMetrics;
        ImGuiStyle const & style = ImGui::GetStyle();
        float const fontSize = ImGui::GetFontSize();
        float const lineHeight = ImGui::GetTextLineHeightWithSpacing();

        // The row is measured before anything is drawn, so its band has a stable height and the
        // next turn starts where this one ended.
        std::size_t const maxLines = turn.kind == TurnKind::Tool
                                       ? kCollapsedToolLines
                                       : (expanded ? 0u : kCollapsedBodyLines);
        float const measuredWidth = maxLines > 0u ? wrapWidth - ellipsisWidth : wrapWidth;
        std::vector<agentWidgets::TextSpan> const lines
          = agentWidgets::measureWrappedLines(turn.body.c_str(), measuredWidth, maxLines);
        bool const truncated
          = maxLines > 0u && !lines.empty()
            && agentWidgets::measureWrappedLines(turn.body.c_str(), measuredWidth).size()
                 > lines.size();

        float const bodyHeight = static_cast<float>(lines.size()) * lineHeight;
        float const imageHeight = turn.hasImage ? lineHeight : 0.f;
        float const roleHeight = showRole ? lineHeight + fontSize * turnRoleLineGap : 0.f;
        float const rowHeight = roleHeight + bodyHeight + imageHeight + fontSize * turnPadding * 2.f;

        agentWidgets::ReservedRow const row = agentWidgets::reserveRow(rowHeight, "##turn");
        bool const clicked = ImGui::IsItemClicked();
        agentWidgets::drawRowBand(row, false, row.hovered);
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        float const contentTop = row.origin.y + fontSize * turnPadding;
        drawRoleChip(drawList,
                     ImVec2(m_chipX, contentTop + lineHeight * 0.5f),
                     fontSize * roleChipRadius,
                     roleColor(turn.kind),
                     turnIcon(turn.kind));

        float y = contentTop;
        if (showRole)
        {
            agentWidgets::drawEmphasized(drawList, ImVec2(m_textX, y), roleColor(turn.kind),
                                         turn.title.c_str());
            y += lineHeight + fontSize * turnRoleLineGap;
        }
        agentWidgets::drawWrappedLines(drawList,
                                       ImVec2(m_textX, y),
                                       lines,
                                       bodyColor(turn.kind),
                                       lineHeight);
        if (truncated)
        {
            agentWidgets::TextSpan const & last = lines.back();
            float const lastY = y + static_cast<float>(lines.size() - 1u) * lineHeight;
            drawList.AddText(ImVec2(m_textX + ImGui::CalcTextSize(last.begin, last.end).x, lastY),
                             ImGui::GetColorU32(bodyColor(turn.kind)),
                             "...");
        }
        if (turn.hasImage)
        {
            agentWidgets::drawClippedText(drawList,
                                          ImVec2(m_textX, y + bodyHeight),
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                          ICON_FA_IMAGE "  image attached",
                                          wrapWidth);
        }

        m_lastTurns.push_back(TurnLayout{
          row.origin, row.max, m_chipX, m_textX, turn.kind, truncated});

        agentWidgets::endReservedRow(row);
        float const breakSpacing = speakerChanged ? turnBreakSpacing : turnSpacing;
        ImGui::Dummy(ImVec2(0.f, fontSize * breakSpacing + style.ItemSpacing.y * 0.5f));
        return clicked;
    }
} // namespace gladius::ui
