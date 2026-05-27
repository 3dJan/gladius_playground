#include "GamepadHintBar.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace gladius::ui
{

    void GamepadHintBar::render() const
    {
        if (!GamepadState::instance().isAnyConnected())
        {
            return;
        }

        auto const & actionMap = GamepadActionMap::instance();

        // The hint entries to display: (action, label)
        struct HintEntry
        {
            GamepadAction action;
            char const * label;
        };

        static constexpr HintEntry entries[] = {
            {GamepadAction::NavigateUp,    "Navigate"},
            {GamepadAction::Select,        "Select"},
            {GamepadAction::Deselect,      "Back"},
            {GamepadAction::ToggleSelect,  "Toggle"},
            {GamepadAction::OpenMenu,      "Menu"},
            {GamepadAction::Undo,          "Undo"},
            {GamepadAction::Redo,          "Redo"},
            {GamepadAction::CenterView,    "Center"},
            {GamepadAction::NavigateBack,  "Prev Fn"},
        };

        ImDrawList * drawList = ImGui::GetWindowDrawList();
        ImVec2 const winPos  = ImGui::GetWindowPos();
        ImVec2 const winSize = ImGui::GetWindowSize();

        // Anchor the bar to the bottom edge of the current window
        float const barY = winPos.y + winSize.y - m_barHeight;
        ImVec2 const barMin(winPos.x, barY);
        ImVec2 const barMax(winPos.x + winSize.x, barY + m_barHeight);

        drawList->AddRectFilled(barMin, barMax, m_backgroundColor);

        float cursorX = barMin.x + m_paddingX;
        float const textY = barY + m_paddingY;

        ImFont * const font = ImGui::GetFont();
        float const fontSize = ImGui::GetFontSize();

        for (auto const & entry : entries)
        {
            std::string const binding = "[" + actionMap.getBindingDisplay(entry.action) + "]";
            std::string const label   = std::string(" ") + entry.label + "  ";

            // Key badge
            ImVec2 const keySize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, binding.c_str());
            drawList->AddText(ImVec2(cursorX, textY), m_keyColor, binding.c_str());
            cursorX += keySize.x;

            // Label
            ImVec2 const labelSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, label.c_str());
            drawList->AddText(ImVec2(cursorX, textY), m_labelColor, label.c_str());
            cursorX += labelSize.x;

            // Stop if we're running out of space
            if (cursorX > barMax.x - m_paddingX * 4.0f)
            {
                break;
            }
        }

        // Connection indicator at far right
        {
            char const * indicator = "● GP";
            ImVec2 const indSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, indicator);
            float const indX = barMax.x - indSize.x - m_paddingX;
            drawList->AddText(ImVec2(indX, textY), IM_COL32(80, 220, 80, 255), indicator);
        }
    }

} // namespace gladius::ui
