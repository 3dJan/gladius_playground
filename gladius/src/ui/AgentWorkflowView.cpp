#include "AgentWorkflowView.h"

#include "AgentWorkflowAnimation.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        constexpr ImVec4 kCompletedColor = {0.36f, 0.66f, 0.42f, 1.00f};
        constexpr ImVec4 kAttentionColor = {0.86f, 0.62f, 0.22f, 1.00f};

        /// @brief Gladius accent colour of the active theme instead of a hard-coded hue
        ImVec4 accentColor()
        {
            return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        }

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
        return ImGui::GetFontSize() * 1.20f;
    }

    void AgentWorkflowView::render(
      std::array<PhaseState, kWorkflowPhaseCount> const & states,
      std::size_t activeIndex,
      std::array<std::string, kWorkflowPhaseCount> const & summaries,
      std::array<bool, kWorkflowPhaseCount> & expanded,
      std::array<ArtifactRenderer, kWorkflowPhaseCount> const & renderers,
      AgentWorkflowAnimationState & animation)
    {
        ImGuiStyle const & style = ImGui::GetStyle();
        float const fontSizePx = ImGui::GetFontSize();
        float const lineHeight = fontSizePx + style.ItemSpacing.y;
        float const smallLine = fontSizePx * 0.95f;
        float const rowStartX = ImGui::GetCursorScreenPos().x;
        float const availWidth = std::max(64.f, ImGui::GetContentRegionAvail().x);
        float const railX = rowStartX + fontSizePx * 0.90f;
        float const contentX = railX + contentIndent();
        float const nowSeconds = static_cast<float>(ImGui::GetTime());

        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        std::array<RowLayout, kWorkflowPhaseCount> rows{};

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            PhaseState const state = states[i];
            float const wrapWidth = std::max(48.f, rowStartX + availWidth - contentX);
            std::vector<TextLine> const summaryLines =
              measureWrappedLines(summaries[i].c_str(), wrapWidth);
            float const summaryHeight = static_cast<float>(summaryLines.size()) * smallLine;
            float const rowHeight = lineHeight + summaryHeight;

            ImGui::SetCursorPosX(rowStartX);
            ImGui::PushID(static_cast<int>(i));
            bool const clicked = ImGui::InvisibleButton("##phaseRow", ImVec2(availWidth, rowHeight));
            RowLayout & row = rows[i];
            row.hovered = ImGui::IsItemHovered();
            row.rowTop = ImGui::GetItemRectMin().y;
            row.rowBottom = ImGui::GetItemRectMax().y;
            ImGui::PopID();

            row.center = ImVec2(railX, (row.rowTop + row.rowBottom) * 0.5f);
            row.radius = state == PhaseState::Active ? fontSizePx * 0.34f : fontSizePx * 0.27f;
            row.expanded = expanded[i];
            row.contentBottom = row.rowBottom;

            if (row.hovered)
            {
                drawList.AddRectFilled(ImVec2(contentX, row.rowTop),
                                       ImVec2(rowStartX + availWidth, row.rowBottom),
                                       toU32(accentColor(), 0.08f),
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
            drawList.AddText(ImVec2(contentX, row.rowTop + style.ItemSpacing.y * 0.5f),
                             ImGui::GetColorU32(titleColor),
                             phaseLabel(phaseAt(i)));
            if (!summaryLines.empty())
            {
                ImVec4 const summaryColor = state == PhaseState::Attention
                                              ? scaled(kAttentionColor, 0.95f)
                                              : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                drawWrappedLines(drawList,
                                 ImVec2(contentX, row.rowTop + lineHeight),
                                 summaryLines,
                                 summaryColor,
                                 smallLine);
            }

            if (row.expanded)
            {
                ImGui::SetCursorPosX(contentX);
                ImGui::Dummy(ImVec2(0.f, fontSizePx * 0.15f));
                ImGui::SetCursorPosX(contentX);
                if (renderers[i])
                {
                    renderers[i]();
                }
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
                ImGui::SetCursorPosX(rowStartX);
                ImGui::Dummy(ImVec2(0.f, fontSizePx * 0.34f));
            }
        }

        // Connectors are drawn after layout so the line can run through the artifact block of the
        // phase above; that keeps the progress line continuous while a phase is expanded.
        for (std::size_t i = 1; i < kWorkflowPhaseCount; ++i)
        {
            float const fromY = rows[i - 1].contentBottom;
            float const toY = rows[i].center.y - rows[i].radius;
            if (toY <= fromY)
            {
                continue;
            }
            bool const travelled = states[i - 1] == PhaseState::Completed
                                   || states[i - 1] == PhaseState::Attention;
            ImVec4 const lineColor = travelled
                                       ? scaled(accentColor(), 0.42f)
                                       : ImGui::GetStyleColorVec4(ImGuiCol_Border);
            drawList.AddLine(ImVec2(railX, fromY), ImVec2(railX, toY),
                             ImGui::GetColorU32(lineColor), 1.4f);
        }

        renderConnector(drawList, rows, activeIndex, animation, nowSeconds);

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            renderCircle(drawList,
                         rows[i],
                         states[i],
                         animation.completionPulse(i, nowSeconds),
                         states[i] == PhaseState::Active ? animation.activeBreath(nowSeconds) : 0.f);
        }
    }

    void AgentWorkflowView::renderCircle(ImDrawList & drawList,
                                         RowLayout const & row,
                                         PhaseState state,
                                         float pulse,
                                         float breath) const
    {
        ImVec2 const c = row.center;
        float const r = row.radius;
        ImVec4 const accent = accentColor();
        float const dpi = ImGui::GetWindowDpiScale();

        if (pulse > 0.001f)
        {
            drawList.AddCircle(c,
                               r + (2.0f + pulse * 5.0f) * dpi,
                               toU32(accent, 0.40f * pulse),
                               32,
                               1.2f * dpi);
        }

        switch (state)
        {
        case PhaseState::Pending:
            drawList.AddCircleFilled(c, r, ImGui::GetColorU32(ImGuiCol_FrameBg));
            drawList.AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Border), 28, 1.2f * dpi);
            break;
        case PhaseState::Completed:
            drawList.AddCircleFilled(c, r, toU32(kCompletedColor, 0.28f));
            drawList.AddCircle(c, r, ImGui::GetColorU32(kCompletedColor), 28, 1.3f * dpi);
            {
                float const s = r * 0.62f;
                ImU32 const ink = ImGui::GetColorU32(kCompletedColor);
                drawList.AddLine(ImVec2(c.x - s, c.y + s * 0.02f),
                                 ImVec2(c.x - s * 0.22f, c.y + s * 0.66f), ink, 1.6f * dpi);
                drawList.AddLine(ImVec2(c.x - s * 0.22f, c.y + s * 0.66f),
                                 ImVec2(c.x + s * 0.90f, c.y - s * 0.66f), ink, 1.6f * dpi);
            }
            break;
        case PhaseState::Attention:
            drawList.AddCircleFilled(c, r, toU32(kAttentionColor, 0.26f));
            drawList.AddCircle(c, r, ImGui::GetColorU32(kAttentionColor), 28, 1.5f * dpi);
            {
                ImU32 const ink = ImGui::GetColorU32(kAttentionColor);
                drawList.AddLine(ImVec2(c.x, c.y - r * 0.48f),
                                 ImVec2(c.x, c.y + r * 0.14f), ink, 1.6f * dpi);
                drawList.AddCircleFilled(ImVec2(c.x, c.y + r * 0.46f), 1.4f * dpi, ink);
            }
            break;
        case PhaseState::Active:
            drawList.AddCircleFilled(c, r, toU32(accent, 0.30f));
            drawList.AddCircle(c, r, ImGui::GetColorU32(accent), 32, 1.8f * dpi);
            // Slow breathing ring: the only continuously animated element in the rail.
            drawList.AddCircle(c,
                               r + (2.5f + breath * 2.0f) * dpi,
                               toU32(accent, 0.12f + 0.16f * breath),
                               32,
                               1.1f * dpi);
            break;
        }
    }

    void AgentWorkflowView::renderConnector(ImDrawList & drawList,
                                            std::array<RowLayout, kWorkflowPhaseCount> const & rows,
                                            std::size_t activeIndex,
                                            AgentWorkflowAnimationState & animation,
                                            float nowSeconds) const
    {
        if (!animation.lineTransitionActive() || activeIndex == 0)
        {
            return;
        }
        std::size_t const from = animation.lineTransitionFrom();
        if (from + 1 >= kWorkflowPhaseCount)
        {
            return;
        }
        std::size_t const to = from + 1;
        float const progress = animation.lineTransitionProgress(nowSeconds);
        if (progress >= 1.0f)
        {
            return;
        }
        float const fromY = rows[from].contentBottom;
        float const toY = rows[to].center.y - rows[to].radius;
        if (toY <= fromY)
        {
            return;
        }
        // A short luminous section travels down the connector into the newly active phase.
        float const centre = fromY + (toY - fromY) * progress;
        float const span = std::min((toY - fromY) * 0.30f, 26.f);
        ImVec4 const accent = accentColor();
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
            drawList.AddLine(ImVec2(rows[from].center.x, y0),
                             ImVec2(rows[to].center.x, y1),
                             toU32(accent, 0.55f * fade),
                             1.8f);
        }
    }
} // namespace gladius::ui
