#include "AgentPanelHeader.h"

#include "AgentPanelMetrics.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>

namespace gladius::ui
{
    namespace
    {
        /// @brief Pseudo-bold: the loaded font has no bold face, so a small offset overdraw does it
        void drawEmphasized(ImDrawList & drawList, ImVec2 pos, ImU32 color, char const * text)
        {
            float const nudge = std::max(0.6f, ImGui::GetFontSize() * 0.035f);
            drawList.AddText(ImVec2(pos.x, pos.y), color, text);
            drawList.AddText(ImVec2(pos.x + nudge, pos.y), color, text);
        }

        /**
         * @brief Starts an interactive window move from the drawn header.
         * @details A docked panel has to be floated first, otherwise the move would drag the whole
         *          dock node. The floated panel can be dragged back onto the dockspace later.
         */
        void startWindowDrag(ImGuiWindow & window)
        {
            if (window.DockNode != nullptr)
            {
                ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), &window);
            }
            ImGui::StartMouseMovingWindow(&window);
        }
    } // namespace

    float AgentPanelHeader::height()
    {
        using namespace agentMetrics;
        return ImGui::GetFontSize() * (headerTitleLine + headerPadding * 2.f)
             + ImGui::GetTextLineHeight();
    }

    AgentPanelHeader::Commands AgentPanelHeader::render(std::string const & goal)
    {
        using namespace agentMetrics;
        Commands commands;
        ImGuiWindow & window = *ImGui::GetCurrentWindow();
        ImDrawList & drawList = *ImGui::GetWindowDrawList();
        ImGuiStyle const & style = ImGui::GetStyle();
        float const fontSize = ImGui::GetFontSize();

        ImVec2 const origin = ImGui::GetCursorScreenPos();
        float const width = ImGui::GetContentRegionAvail().x;
        float const headerHeight = height();
        float const pad = fontSize * headerPadding;
        float const menuSize = fontSize * 1.50f;
        float const titleRowY = origin.y + pad;
        float const goalRowY = titleRowY + fontSize * headerTitleLine;

        drawList.AddRectFilled(origin,
                               ImVec2(origin.x + width, origin.y + headerHeight),
                               ImGui::GetColorU32(ImGuiCol_TitleBg),
                               style.WindowRounding,
                               ImDrawFlags_RoundCornersTop);
        drawEmphasized(drawList,
                       ImVec2(origin.x + pad, titleRowY),
                       ImGui::GetColorU32(ImGuiCol_Text),
                       "Design Agent");

        // --- Drag handle: what the native title bar used to provide -------------------
        // ButtonBehavior with PressedOnClick is required here: the move has to start while the
        // mouse is still down, which a clicked InvisibleButton is too late for.
        ImVec2 const dragSize(std::max(0.f, width - menuSize - pad * 2.f),
                              fontSize * headerTitleLine);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, titleRowY));
        ImGui::ItemSize(dragSize);
        ImVec2 const dragMin(origin.x + pad, titleRowY);
        ImRect const dragRect(dragMin, ImVec2(dragMin.x + dragSize.x, dragMin.y + dragSize.y));
        bool hovered = false;
        bool held = false;
        if (ImGui::ButtonBehavior(dragRect, ImGui::GetID("##headerDrag"), &hovered, &held,
                                  ImGuiButtonFlags_PressedOnClick))
        {
            startWindowDrag(window);
        }
        if (hovered)
        {
            ImGui::SetTooltip("Drag to move the panel");
        }

        // --- Goal row: click to edit --------------------------------------------------
        float const goalWidth = std::max(40.f, width - pad * 2.f);
        if (m_goalEditing)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, goalRowY));
            ImGui::PushItemWidth(goalWidth);
            if (m_focusGoalInput)
            {
                ImGui::SetKeyboardFocusHere(0);
                m_focusGoalInput = false;
            }
            ImGui::InputText("##goal", m_goalBuf.data(), m_goalBuf.size());
            bool const committed = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopItemWidth();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || committed)
            {
                m_goalEditing = false;
                if (committed)
                {
                    commands.goalCommitRequested = true;
                    commands.goalDraft = m_goalBuf.data();
                }
            }
        }
        else
        {
            bool const empty = goal.empty();
            char const * text = empty ? "Describe what you want to design" : goal.c_str();
            ImU32 const color = empty ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                                      : ImGui::GetColorU32(ImGuiCol_Text);
            drawList.PushClipRect(ImVec2(origin.x + pad, goalRowY),
                                  ImVec2(origin.x + width, goalRowY + ImGui::GetTextLineHeight()),
                                  true);
            drawList.AddText(ImVec2(origin.x + pad, goalRowY), color, text);
            drawList.PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, goalRowY));
            if (ImGui::InvisibleButton("##goalRow", ImVec2(goalWidth, ImGui::GetTextLineHeight())))
            {
                std::snprintf(m_goalBuf.data(), m_goalBuf.size(), "%s", goal.c_str());
                m_goalEditing = true;
                m_focusGoalInput = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Click to edit the design goal");
            }
        }

        // --- Kebab menu ---------------------------------------------------------------
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - menuSize - pad * 0.5f, titleRowY));
        bool const menuClicked = ImGui::InvisibleButton("##agentMenu",
                                                        ImVec2(menuSize, fontSize * 1.25f));
        if (ImGui::IsItemHovered())
        {
            drawList.AddRectFilled(ImGui::GetItemRectMin(),
                                   ImGui::GetItemRectMax(),
                                   ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                   style.FrameRounding);
        }
        drawList.AddText(ImVec2(origin.x + width - menuSize - pad * 0.5f + menuSize * 0.32f,
                                titleRowY + fontSize * 0.10f),
                         ImGui::GetColorU32(ImGuiCol_Text),
                         ICON_FA_ELLIPSIS_V);
        if (menuClicked)
        {
            ImGui::OpenPopup("agentMenuPopup");
        }

        // --- Divider and cursor hand-off ----------------------------------------------
        float const dividerY = origin.y + headerHeight - fontSize * 0.10f;
        drawList.AddLine(ImVec2(origin.x, dividerY),
                         ImVec2(origin.x + width, dividerY),
                         ImGui::GetColorU32(ImGuiCol_Separator),
                         1.f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + headerHeight));

        if (ImGui::BeginPopup("agentMenuPopup"))
        {
            if (ImGui::MenuItem(ICON_FA_COG " Settings"))
            {
                commands.settingsRequested = true;
            }
            if (ImGui::MenuItem(ICON_FA_ERASER " Clear session"))
            {
                commands.clearSessionRequested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_EYE_SLASH " Hide panel"))
            {
                commands.hideRequested = true;
            }
            ImGui::EndPopup();
        }

        return commands;
    }
} // namespace gladius::ui
