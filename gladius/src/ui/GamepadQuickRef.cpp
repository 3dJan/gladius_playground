#include "GamepadQuickRef.h"

#include "GamepadActionMap.h"

#include <imgui.h>

namespace gladius::ui
{

    void GamepadQuickRef::show()
    {
        m_visible = true;
    }

    void GamepadQuickRef::hide()
    {
        m_visible = false;
    }

    bool GamepadQuickRef::isVisible() const
    {
        return m_visible;
    }

    void GamepadQuickRef::toggle()
    {
        m_visible = !m_visible;
    }

    void GamepadQuickRef::render()
    {
        if (!m_visible)
        {
            return;
        }

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse       |
            ImGuiWindowFlags_NoSavedSettings;

        if (!ImGui::Begin("Gamepad Quick Reference", &m_visible, flags))
        {
            ImGui::End();
            return;
        }

        auto const & actionMap = GamepadActionMap::instance();
        auto const actions = actionMap.getAllActions();

        if (ImGui::BeginTable("QuickRefTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Button",  ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();

            for (auto const & [action, name] : actions)
            {
                if (action == GamepadAction::Count)
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(actionMap.getBindingDisplay(action).c_str());
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

} // namespace gladius::ui
