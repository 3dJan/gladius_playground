#include "PromptInput.h"
#include "PromptInputShared.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        ImVec4 transparent(ImVec4 color)
        {
            color.w = 0.f;
            return color;
        }

        /// @brief Soft fill for the submit button; alpha keeps it subordinate to the field
        ImVec4 accentTint(float alpha = 0.16f)
        {
            ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
            color.w = alpha;
            return color;
        }
    } // namespace

    PromptInput::PromptInput(Config config)
      : m_config(config)
    {
    }

    std::string PromptInput::takeDraft()
    {
        return std::exchange(m_draft, std::string{});
    }

    void PromptInput::setDraft(std::string draft)
    {
        m_draft = std::move(draft);
    }

    bool PromptInput::appendTextFile(std::filesystem::path const & path, std::string & outError)
    {
        outError.clear();
        if (!isSupportedTextFile(path))
        {
            outError = "Unsupported file type: " + path.filename().string();
            return false;
        }

        std::string content;
        if (readTextFile(path, m_config.maxImportBytes, content, outError) == 0u)
        {
            if (outError.empty())
            {
                outError = "File is empty: " + path.filename().string();
            }
            return false;
        }
        if (!isTextContent(content))
        {
            outError = "File is not plain text: " + path.filename().string();
            return false;
        }

        appendToDraft(m_draft, content);
        return true;
    }








    PromptInput::RenderResult PromptInput::renderCard(CardMetrics const & metrics,
                                                      SlotRenderer const & attachments,
                                                      ToolbarRenderer const & toolbar,
                                                      bool sendEnabled)
    {
        RenderResult result;
        ImGuiStyle const & style = ImGui::GetStyle();
        bool const chordPressed = m_lastInlineFocused && sendEnabled && isSendChordPressed();
        float const rowHeight = toolbarHeight();
        float const cardHeight = metrics.cardHeight > 0.f
                                   ? metrics.cardHeight
                                   : measureCard(0.f).cardHeight;

        ImGui::PushID("promptComposer");
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, m_config.cardRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, kCardBorderThickness);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(m_config.cardPadding, m_config.cardPadding));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_FrameBg]);
        // The focused card reads as an active field: the accent border replaces the plain one.
        ImGui::PushStyleColor(ImGuiCol_Border,
                              m_lastInlineFocused ? style.Colors[ImGuiCol_SliderGrab]
                                                  : style.Colors[ImGuiCol_Border]);

        if (ImGui::BeginChild("##card",
                              ImVec2(cardOuterWidth(), cardHeight),
                              ImGuiChildFlags_Borders))
        {
            float const innerWidth = std::max(40.f, ImGui::GetContentRegionAvail().x);

            // --- field: borderless on the card surface, placeholder behind the caret ---
            ImGui::PushStyleColor(ImGuiCol_FrameBg, transparent(style.Colors[ImGuiCol_FrameBg]));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                                  transparent(style.Colors[ImGuiCol_FrameBg]));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                                  transparent(style.Colors[ImGuiCol_FrameBg]));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);

            if (shouldShowPlaceholder(m_draft, m_config.placeholder))
            {
                ImVec2 const frameOrigin = ImGui::GetCursorScreenPos();
                ImVec2 const textPos(frameOrigin.x + style.FramePadding.x,
                                     frameOrigin.y + style.FramePadding.y);
                ImGui::GetWindowDrawList()->AddText(textPos,
                                                    ImGui::GetColorU32(
                                                      style.Colors[ImGuiCol_TextDisabled]),
                                                    m_config.placeholder);
            }

            ImGui::InputTextMultiline("##field",
                                      &m_draft,
                                      ImVec2(innerWidth, metrics.fieldHeight));
            result.focused = ImGui::IsItemFocused();
            if (result.focused)
            {
                m_activeFieldId = ImGui::GetItemID();
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            // --- attachment strip ---
            if (attachments)
            {
                attachments(innerWidth);
            }

            ImGui::Separator();

            // --- toolbar: host controls left, expand and submit right ---
            ToolbarLayout layout = computeToolbarLayout(innerWidth,
                                                        m_config.iconButtonSize,
                                                        m_config.leadingToolbarWidth,
                                                        m_config.minComboWidth,
                                                        style.ItemSpacing.x);

            for (int row = 0; row < layout.rowCount; ++row)
            {
                bool const lastRow = row + 1 == layout.rowCount;
                if (row > 0)
                {
                    // Row 0 ends with an item (the host's leading controls), so a line break
                    // puts this row's controls on their own line.
                    ImGui::NewLine();
                }

                ToolbarLayout rowLayout = layout;
                rowLayout.leadingWidth = row == 0 ? m_config.leadingToolbarWidth : 0.f;
                rowLayout.drawLeading = row == 0;
                rowLayout.drawCombos = lastRow;
                rowLayout.rowHeight = rowHeight;
                rowLayout.comboWidth = computeComboWidth(rowLayout.hostWidth,
                                                         rowLayout.leadingWidth,
                                                         style.ItemSpacing.x);

                if (toolbar)
                {
                    toolbar(rowLayout, row);
                }

                if (!lastRow)
                {
                    continue;
                }

                // Right-align the two icon buttons against the card's inner edge.
                float const contentMaxX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(contentMaxX - trailingToolbarWidth());

                if (ImGui::Button(m_expandedOpen ? ICON_FA_WINDOW_MINIMIZE
                                                 : ICON_FA_WINDOW_MAXIMIZE,
                                  ImVec2(m_config.iconButtonSize, rowHeight)))
                {
                    m_expandedOpen = !m_expandedOpen;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Large prompt editor\n"
                                      "Drop .txt/.md files for long instructions");
                }

                ImGui::SameLine();
                ImGui::BeginDisabled(!sendEnabled);
                // The submit button is the one accent-coloured control of the footer: it marks the
                // primary action without needing a text label.
                ImGui::PushStyleColor(ImGuiCol_Button, transparent(accentTint()));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentTint());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentTint(0.95f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      m_lastInlineFocused
                                        ? style.Colors[ImGuiCol_SliderGrab]
                                        : style.Colors[ImGuiCol_Text]);
                bool const sendClicked
                  = ImGui::Button(ICON_FA_PAPER_PLANE, ImVec2(m_config.iconButtonSize, rowHeight));
                ImGui::PopStyleColor(4);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", sendChordHelp());
                }
                ImGui::EndDisabled();

                result.sendRequested = sendClicked || chordPressed;
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGui::PopID();

        m_lastInlineFocused = result.focused;
        return result;
    }

    std::string PromptInput::consumeDraft()
    {
        // While focused, InputText owns a private copy of the text and re-applies it to
        // our buffer, so the submitted prompt must be dropped from there as well.
        if (ImGuiInputTextState * state = ImGui::GetInputTextState(m_activeFieldId))
        {
            state->ClearText();
        }
        ImGui::ClearActiveID();

        // Discard the snapshot InputText stores on deactivation, otherwise the cleared
        // prompt is written back into the draft on the next frame.
        ImGuiContext & g = *GImGui;
        if (g.InputTextDeactivatedState.ID == m_activeFieldId)
        {
            g.InputTextDeactivatedState.ID = 0;
        }

        return takeDraft();
    }

} // namespace gladius::ui
