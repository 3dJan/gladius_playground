#include "AgentPanelWidgets.h"

#include "AgentPanelMetrics.h"
#include "AgentWorkflowRailDraw.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gladius::ui
{
    namespace agentWidgets
    {
        namespace
        {
            /// @brief ImGui has no ImVec4+alpha overload of GetColorU32
            ImU32 tinted(ImVec4 color, float alpha)
            {
                color.w *= alpha;
                return ImGui::GetColorU32(color);
            }

            void drawValue(char const * hint, char * valueBuf, std::size_t valueBufSize)
            {
                float const available = ImGui::GetContentRegionAvail().x;
                ImGui::SetNextItemWidth(available);
                if (hint != nullptr)
                {
                    ImGui::InputTextWithHint("##value", hint, valueBuf, valueBufSize);
                }
                else
                {
                    ImGui::InputText("##value", valueBuf, valueBufSize);
                }
            }

            /// @brief True when the byte at p starts a UTF-8 continuation character
            bool isContinuation(char c)
            {
                return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
            }

            /// @brief Moves p back to the start of a UTF-8 sequence
            char const * alignToCharacterStart(char const * p, char const * textBegin)
            {
                while (p > textBegin && isContinuation(*p))
                {
                    --p;
                }
                return p;
            }
        } // namespace

        ImVec4 accent()
        {
            return railAccentColor();
        }

        std::vector<TextSpan> measureWrappedLines(char const * text,
                                                  float wrapWidth,
                                                  std::size_t maxLines)
        {
            std::vector<TextSpan> spans;
            if (text == nullptr || text[0] == '\0' || wrapWidth <= 0.f)
            {
                return spans;
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
                    char const * wrapEnd
                      = font->CalcWordWrapPositionA(scale, lineStart, paragraphEnd, wrapWidth);
                    if (wrapEnd == nullptr || wrapEnd <= lineStart)
                    {
                        wrapEnd = paragraphEnd;
                    }
                    spans.push_back(TextSpan{lineStart, wrapEnd});
                    lineStart = wrapEnd;
                } while (lineStart < paragraphEnd);

                if (paragraphEnd >= textEnd)
                {
                    break;
                }
                paragraph = paragraphEnd + 1;
            }

            if (maxLines > 0u && spans.size() > maxLines)
            {
                spans.resize(maxLines);
            }
            return spans;
        }

        void drawWrappedLines(ImDrawList & drawList,
                              ImVec2 pos,
                              std::vector<TextSpan> const & lines,
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

        void drawClippedText(ImDrawList & drawList,
                             ImVec2 pos,
                             ImVec4 color,
                             char const * text,
                             float maxWidth)
        {
            if (text == nullptr || text[0] == '\0' || maxWidth <= 0.f)
            {
                return;
            }
            ImFont * font = ImGui::GetFont();
            float const scale = ImGui::GetFontSize() / font->FontSize;
            char const * const textEnd = text + std::strlen(text);
            if (ImGui::CalcTextSize(text, textEnd).x <= maxWidth)
            {
                drawList.AddText(font, ImGui::GetFontSize(), pos, ImGui::GetColorU32(color), text);
                return;
            }
            char const * cut = font->CalcWordWrapPositionA(scale, text, textEnd, maxWidth);
            if (cut == nullptr || cut <= text)
            {
                cut = textEnd;
            }
            // Leave room for the ellipsis itself.
            float const ellipsisWidth = ImGui::CalcTextSize("...").x;
            while (cut > text
                   && ImGui::CalcTextSize(text, alignToCharacterStart(cut, text)).x
                        > maxWidth - ellipsisWidth)
            {
                cut = alignToCharacterStart(cut, text);
                cut = cut > text ? cut - 1 : text;
            }
            char const * shownEnd = alignToCharacterStart(cut, text);
            if (shownEnd <= text)
            {
                shownEnd = text;
            }
            drawList.AddText(font, ImGui::GetFontSize(), pos, ImGui::GetColorU32(color), text,
                             shownEnd);
            drawList.AddText(ImVec2(pos.x + ImGui::CalcTextSize(text, shownEnd).x, pos.y),
                             ImGui::GetColorU32(color),
                             "...");
        }

        void drawEmphasized(ImDrawList & drawList, ImVec2 pos, ImVec4 color, char const * text)
        {
            float const nudge = std::max(0.6f, ImGui::GetFontSize() * 0.035f);
            ImU32 const col = ImGui::GetColorU32(color);
            drawList.AddText(ImVec2(pos.x, pos.y), col, text);
            drawList.AddText(ImVec2(pos.x + nudge, pos.y), col, text);
        }

        void sectionLabel(char const * icon, char const * label, bool chevronDown)
        {
            using namespace agentMetrics;
            ImDrawList & drawList = *ImGui::GetWindowDrawList();
            ImGuiStyle const & style = ImGui::GetStyle();
            float const fontSize = ImGui::GetFontSize();
            ImVec2 const pos = ImGui::GetCursorScreenPos();
            float const available = ImGui::GetContentRegionAvail().x;
            float const iconWidth = ImGui::CalcTextSize(icon).x;
            float const chevronWidth = chevronDown ? fontSize * 1.2f : 0.f;

            drawList.AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), icon);
            float const labelX = pos.x + iconWidth + style.ItemInnerSpacing.x;
            float const limit = std::max(0.f, available - iconWidth
                                                 - style.ItemInnerSpacing.x - chevronWidth);
            drawClippedText(drawList, ImVec2(labelX, pos.y), ImGui::GetStyleColorVec4(ImGuiCol_Text),
                            label, limit);
            if (chevronDown)
            {
                drawList.AddText(ImVec2(pos.x + available - fontSize * 1.1f, pos.y),
                                 ImGui::GetColorU32(ImGuiCol_Text),
                                 ICON_FA_CHEVRON_DOWN);
            }

            // The label is drawn, not submitted as a widget, so the row is reserved by hand.
            ImGui::ItemSize(ImVec2(available, ImGui::GetTextLineHeight()));
        }

        bool toggleRow(char const * id, char const * icon, char const * label, bool & open)
        {
            float const fontSize = ImGui::GetFontSize();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fontSize * 0.10f);
            // The whole row is the hit area, so it has to stay finger sized on a compact surface.
            float const rowHeight
              = std::max(ImGui::GetTextLineHeight(), minTouchTargetHeight());
            bool const clicked
              = ImGui::InvisibleButton(id, ImVec2(ImGui::GetContentRegionAvail().x, rowHeight));
            if (clicked)
            {
                open = !open;
            }
            ImDrawList & drawList = *ImGui::GetWindowDrawList();
            ImVec2 const min = ImGui::GetItemRectMin();
            drawList.AddText(ImVec2(min.x, min.y),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
            drawList.AddText(ImVec2(min.x + fontSize * 1.25f, min.y),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             icon);
            drawList.AddText(ImVec2(min.x + fontSize * 1.25f + ImGui::CalcTextSize(icon).x
                                        + ImGui::GetStyle().ItemInnerSpacing.x,
                                    min.y),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             label);
            return open;
        }

        float beginKeyColumn(char const * key)
        {
            using namespace agentMetrics;
            float const fontSize = ImGui::GetFontSize();
            float const columnWidth = fontSize * keyColumnWidth;
            float const rowWidth = ImGui::GetContentRegionAvail().x;
            char const * shown = key != nullptr && key[0] != '\0' ? key : "Note";

            // The key is right-aligned against a fixed column, so the value controls of a whole
            // section share one left edge.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, rowWidth - columnWidth));
            float const keyTextX = ImGui::GetCursorPosX()
                                 + std::max(0.f, columnWidth - ImGui::CalcTextSize(shown).x);
            ImGui::SetCursorPosX(keyTextX);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::TextUnformatted(shown);
            ImGui::PopStyleColor();
            return std::max(0.f, rowWidth - columnWidth);
        }

        bool keyValueRow(char const * key, char * valueBuf, std::size_t valueBufSize)
        {
            beginKeyColumn(key);
            ImGui::SameLine();
            drawValue(nullptr, valueBuf, valueBufSize);
            return ImGui::IsItemDeactivatedAfterEdit();
        }

        bool keyValueRowWithHint(char const * key,
                                 char const * hint,
                                 char * valueBuf,
                                 std::size_t valueBufSize)
        {
            beginKeyColumn(key);
            ImGui::SameLine();
            drawValue(hint, valueBuf, valueBufSize);
            return ImGui::IsItemDeactivatedAfterEdit();
        }

        ReservedRow reserveRow(float height, char const * id)
        {
            ReservedRow row;
            row.origin = ImGui::GetCursorScreenPos();
            float const width = std::max(40.f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton(id, ImVec2(width, height));
            row.hovered = ImGui::IsItemHovered();
            row.max = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(row.origin);
            return row;
        }

        void endReservedRow(ReservedRow const & row)
        {
            ImGui::SetCursorScreenPos(ImVec2(row.origin.x, row.max.y));
        }

        void drawRowBand(ReservedRow const & row, bool selected, bool hovered)
        {
            drawBand(*ImGui::GetWindowDrawList(), row.origin, row.max, selected, hovered);
        }

        void drawBand(ImDrawList & drawList, ImVec2 min, ImVec2 max, bool selected, bool hovered)
        {
            if (!selected && !hovered)
            {
                return;
            }
            float const fontSize = ImGui::GetFontSize();
            float const nudge = fontSize * 0.25f;
            ImVec2 const bandMin(min.x - nudge, min.y - 1.f);
            ImVec2 const bandMax(max.x + nudge, max.y + 1.f);
            ImVec4 color = selected ? accent() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            color.w *= selected ? 0.20f : 0.06f;
            drawList.AddRectFilled(bandMin,
                                   bandMax,
                                   ImGui::GetColorU32(color),
                                   fontSize * agentMetrics::rowRounding);
            if (selected)
            {
                drawList.AddRect(bandMin,
                                 bandMax,
                                 tinted(accent(), 0.55f),
                                 fontSize * agentMetrics::rowRounding,
                                 0,
                                 1.f);
            }
        }

        void drawRowBand(bool selected, bool hovered)
        {
            drawBand(*ImGui::GetWindowDrawList(),
                     ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax(),
                     selected,
                     hovered);
        }

        void drawCircularThumb(ImDrawList & drawList,
                               ImVec2 center,
                               float radius,
                               unsigned int textureId)
        {
            if (textureId != 0u)
            {
                // AddImageRounded with full rounding gives the circular crop without a clip stack.
                drawList.AddImageRounded(
                  static_cast<ImTextureID>(static_cast<std::uintptr_t>(textureId)),
                  ImVec2(center.x - radius, center.y - radius),
                  ImVec2(center.x + radius, center.y + radius),
                  ImVec2(0.f, 0.f),
                  ImVec2(1.f, 1.f),
                  IM_COL32(255, 255, 255, 255),
                  radius);
                drawList.AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), 40, 1.f);
                return;
            }

            // No preview has been rendered for this item: say so instead of inventing one.
            drawList.AddCircleFilled(center, radius, ImGui::GetColorU32(ImGuiCol_FrameBg), 40);
            drawList.AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), 40, 1.2f);
            char const * mark = ICON_FA_IMAGE;
            ImVec2 const markSize = ImGui::CalcTextSize(mark);
            drawList.AddText(ImVec2(center.x - markSize.x * 0.5f, center.y - markSize.y * 0.5f),
                             ImGui::GetColorU32(ImGuiCol_TextDisabled),
                             mark);
        }

        void drawChip(ImDrawList & drawList, ImVec2 min, ImVec2 max, ImVec4 color, bool hovered)
        {
            using namespace agentMetrics;
            float const fontSize = ImGui::GetFontSize();
            // A chip is a label with a border, not a status flag: the colour only tints the frame,
            // so a mode readout never competes with the warning lines around it.
            float const boost = hovered ? 1.8f : 1.f;
            drawList.AddRectFilled(min,
                                   max,
                                   tinted(color, std::min(1.f, chipFillAlpha * boost)),
                                   fontSize * chipRounding);
            drawList.AddRect(min,
                             max,
                             tinted(color, std::min(1.f, chipBorderAlpha * boost)),
                             fontSize * chipRounding,
                             0,
                             1.f);
        }

        void drawLetterChip(ImDrawList & drawList,
                            ImVec2 center,
                            float radius,
                            char letter,
                            ImVec4 color)
        {
            drawList.AddCircleFilled(center, radius, ImGui::GetColorU32(color), 28);
            char const text[2] = {letter, '\0'};
            ImVec2 const size = ImGui::CalcTextSize(text);
            drawList.AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f),
                             ImGui::GetColorU32(ImGuiCol_Text),
                             text);
        }
    } // namespace agentWidgets
} // namespace gladius::ui
