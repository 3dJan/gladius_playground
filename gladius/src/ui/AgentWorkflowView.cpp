#include "AgentWorkflowView.h"

#include <IconsFontAwesome5.h>

#include "AgentPanelMetrics.h"
#include "AgentWorkflowAnimation.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        ImVec4 scaled(ImVec4 color, float alpha)
        {
            color.w *= alpha;
            return color;
        }

        ImU32 toU32(ImVec4 color, float alpha)
        {
            return ImGui::GetColorU32(scaled(color, alpha));
        }

        /// @brief One soft-wrapped line of a string, as a [begin,end) span into the original text
        struct TextLine
        {
            char const * begin = nullptr;
            char const * end = nullptr;
        };

        /**
         * @brief Splits text into soft-wrapped lines for the current font at the given width.
         * @details Layout is measured up front so the rail can reserve the exact row height: the
         *          phase rows therefore never shift when their status text changes.
         */
        std::vector<TextLine> measureWrappedLines(char const * text, float wrapWidth)
        {
            std::vector<TextLine> lines;
            if (text == nullptr || text[0] == '\0' || wrapWidth <= 0.f)
            {
                return lines;
            }
            ImFont * font = ImGui::GetFont();
            float const scale = ImGui::GetFontSize() / font->FontSize;
            char const * const textEnd = text + std::strlen(text);
            char const * paragraph = text;
            while (true)
            {
                char const * const paragraphEnd = std::find(paragraph, textEnd, '\n');
                char const * lineStart = paragraph;
                // Each pass consumes at least one character: the wrap point is clamped forward.
                do
                {
                    char const * wrapEnd =
                      font->CalcWordWrapPositionA(scale, lineStart, paragraphEnd, wrapWidth);
                    if (wrapEnd == nullptr || wrapEnd <= lineStart)
                    {
                        wrapEnd = paragraphEnd;
                    }
                    lines.push_back(TextLine{lineStart, wrapEnd});
                    lineStart = wrapEnd;
                } while (lineStart < paragraphEnd);

                if (paragraphEnd >= textEnd)
                {
                    break;
                }
                paragraph = paragraphEnd + 1;
            }
            return lines;
        }

        void drawWrappedLines(ImDrawList & drawList,
                              ImVec2 pos,
                              std::vector<TextLine> const & lines,
                              ImVec4 color,
                              float lineHeight)
        {
            ImFont * font = ImGui::GetFont();
            float const size = ImGui::GetFontSize();
            ImU32 const col = ImGui::GetColorU32(color);
            float y = pos.y;
            for (auto const & line : lines)
            {
                if (line.end > line.begin)
                {
                    drawList.AddText(font, size, ImVec2(pos.x, y), col, line.begin, line.end);
                }
                y += lineHeight;
            }
        }
    } // namespace


    float AgentWorkflowView::contentIndent() noexcept
    {
        return ImGui::GetFontSize() * (agentMetrics::railTextGap + agentMetrics::sectionIndent);
    }

    void AgentWorkflowView::render(
      std::array<PhaseState, kWorkflowPhaseCount> const & states,
      std::size_t activeIndex,
      std::array<std::string, kWorkflowPhaseCount> const & summaries,
      std::array<bool, kWorkflowPhaseCount> & expanded,
      std::array<ArtifactRenderer, kWorkflowPhaseCount> const & renderers,
      AgentWorkflowAnimationState & animation)
    {
        using namespace agentMetrics;
        ImGuiStyle const & style = ImGui::GetStyle();
        float const fontSizePx = ImGui::GetFontSize();
        float const lineHeight = fontSizePx + style.ItemSpacing.y;
        float const smallLine = fontSizePx * 0.95f;
        float const nowSeconds = static_cast<float>(ImGui::GetTime());

        // Geometry is kept in screen coordinates end to end: mixing them with the window-local
        // variants is what previously pushed artifact content out to the window's left edge.
        float const rowStartX = ImGui::GetCursorScreenPos().x;
        float const availWidth = std::max(64.f, ImGui::GetContentRegionAvail().x);
        float const rightEdgeX = rowStartX + availWidth;
        float const railX = rowStartX + fontSizePx * railOffset;
        float const textX = railX + fontSizePx * railTextGap;
        float const titleX = textX + fontSizePx * chevronGutter;
        // Pixel aligned: the content column is reached through a group indent, and ImGui snaps the
        // line start of an indented line to the whole pixel of (window pos + indent).
        float const contentX = std::floor(titleX + fontSizePx * sectionIndent);

        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        std::array<RowLayout, kWorkflowPhaseCount> & rows = m_rows;
        rows = {};

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            PhaseState const state = states[i];
            float const wrapWidth = std::max(48.f, rightEdgeX - titleX);
            std::vector<TextLine> const summaryLines =
              measureWrappedLines(summaries[i].c_str(), wrapWidth);
            float const summaryHeight = static_cast<float>(summaryLines.size()) * smallLine;
            float const rowHeight = lineHeight + summaryHeight + fontSizePx * rowPadding;

            ImGui::SetCursorScreenPos(ImVec2(rowStartX, ImGui::GetCursorScreenPos().y));
            ImGui::PushID(static_cast<int>(i));
            bool const clicked = ImGui::InvisibleButton("##phaseRow", ImVec2(availWidth, rowHeight));
            RowLayout & row = rows[i];
            row.hovered = ImGui::IsItemHovered();
            row.rowTop = ImGui::GetItemRectMin().y;
            row.rowBottom = ImGui::GetItemRectMax().y;
            ImGui::PopID();

            row.railX = railX;
            row.contentX = contentX;
            row.radius = railCircleRadius(state);
            row.center = ImVec2(railX, (row.rowTop + row.rowBottom) * 0.5f);
            row.expanded = expanded[i];
            row.contentBottom = row.rowBottom;

            if (row.hovered)
            {
                drawList.AddRectFilled(ImVec2(textX, row.rowTop),
                                       ImVec2(rightEdgeX, row.rowBottom),
                                       toU32(railAccentColor(), 0.07f),
                                       style.FrameRounding * 0.5f);
            }

            ImVec4 titleColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            switch (state)
            {
            case PhaseState::Pending:
                titleColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                break;
            case PhaseState::Active:
                titleColor = ImVec4(0.99f, 0.99f, 1.00f, 1.00f);
                break;
            case PhaseState::Attention:
                titleColor = ImVec4(0.98f, 0.86f, 0.66f, 1.00f);
                break;
            case PhaseState::Completed:
                titleColor = scaled(titleColor, 0.88f);
                break;
            }
            // The chevron is the expand/collapse affordance of the row.
            drawList.AddText(ImVec2(textX, row.rowTop + style.ItemSpacing.y * 0.5f),
                             ImGui::GetColorU32(ImGuiCol_TextDisabled),
                             expanded[i] ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
            drawList.AddText(ImVec2(titleX, row.rowTop + style.ItemSpacing.y * 0.5f),
                             ImGui::GetColorU32(titleColor),
                             phaseLabel(phaseAt(i)));
            if (!summaryLines.empty())
            {
                ImVec4 const summaryColor = state == PhaseState::Attention
                                              ? scaled(railAttentionColor(), 0.95f)
                                              : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                drawWrappedLines(drawList,
                                 ImVec2(titleX, row.rowTop + lineHeight),
                                 summaryLines,
                                 summaryColor,
                                 smallLine);
            }

            if (row.expanded)
            {
                // A real indent, not a one-off cursor move: widgets that reset the line (Text,
                // InputText, a drawn label) return to the indent edge, so every artifact row of a
                // phase shares the content column instead of snapping back to the window's left edge.
                ImGui::Indent(contentX - rowStartX);
                ImGui::Dummy(ImVec2(0.f, fontSizePx * 0.15f));
                if (renderers[i])
                {
                    renderers[i]();
                }
                ImGui::Unindent(contentX - rowStartX);
                row.contentBottom = std::max(row.rowBottom, ImGui::GetCursorScreenPos().y);
            }

            if (clicked)
            {
                expanded[i] = !expanded[i];
            }
            if (row.hovered)
            {
                ImGui::SetTooltip("%s - %s", phaseLabel(phaseAt(i)), phaseHint(phaseAt(i)));
            }

            if (i + 1 < kWorkflowPhaseCount)
            {
                ImGui::SetCursorScreenPos(ImVec2(rowStartX, ImGui::GetCursorScreenPos().y));
                ImGui::Dummy(ImVec2(0.f, fontSizePx * railRowSpacing));
            }
        }

        // The spine is one continuous line from the first node centre to the last one. It is drawn
        // before the nodes, which paint an opaque disc, so the line visibly runs through them.
        for (std::size_t i = 1; i < kWorkflowPhaseCount; ++i)
        {
            bool const travelled = states[i - 1] == PhaseState::Completed
                                   || states[i - 1] == PhaseState::Attention;
            // Centre to centre, never edge to edge: the node discs hide the stroke inside the
            // circle, which is what makes the spine read as one line running through the rail.
            m_spine[i - 1] = {rows[i - 1].center.y, rows[i].center.y};
            drawRailLeg(drawList, railX, m_spine[i - 1].first, m_spine[i - 1].second, travelled);
        }

        renderGlow(drawList, rows, animation, nowSeconds);

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            drawRailCircle(drawList,
                           RailCircle{rows[i].center, rows[i].radius},
                           states[i],
                           animation.completionPulse(i, nowSeconds),
                           states[i] == PhaseState::Active ? animation.activeBreath(nowSeconds) : 0.f);
        }
    }

    void AgentWorkflowView::renderGlow(ImDrawList & drawList,
                                       std::array<RowLayout, kWorkflowPhaseCount> const & rows,
                                       AgentWorkflowAnimationState & animation,
                                       float nowSeconds) const
    {
        if (!animation.lineTransitionActive())
        {
            return;
        }
        std::size_t const from = animation.lineTransitionFrom();
        if (from + 1 >= kWorkflowPhaseCount)
        {
            return;
        }
        float const progress = animation.lineTransitionProgress(nowSeconds);
        if (progress >= 1.0f)
        {
            return;
        }
        float const fromY = rows[from].center.y;
        float const toY = rows[from + 1].center.y;
        if (toY <= fromY)
        {
            return;
        }
        // A short luminous section travels down the spine into the newly active phase.
        float const railX = rows[from].center.x;
        float const centre = fromY + (toY - fromY) * progress;
        float const span = std::min((toY - fromY) * 0.30f, 26.f);
        ImVec4 const accent = railAccentColor();
        for (int s = 0; s < 5; ++s)
        {
            float const t = static_cast<float>(s) / 4.f;
            float const y0 = std::max(fromY, centre - span * (1.0f - t));
            float const y1 = std::min(toY, centre + span * 0.12f);
            if (y1 <= y0)
            {
                continue;
            }
            float const fade = (1.0f - t) * (1.0f - progress * 0.35f);
            drawList.AddLine(ImVec2(railX, y0), ImVec2(railX, y1), toU32(accent, 0.55f * fade), 1.8f);
        }
    }
} // namespace gladius::ui
