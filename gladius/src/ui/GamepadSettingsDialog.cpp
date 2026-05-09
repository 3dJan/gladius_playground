#include "GamepadSettingsDialog.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace gladius::ui
{
    namespace
    {
        std::string actionToJsonKey(GamepadAction action)
        {
            switch (action)
            {
                case GamepadAction::NavigateUp: return "navigate_up";
                case GamepadAction::NavigateDown: return "navigate_down";
                case GamepadAction::NavigateLeft: return "navigate_left";
                case GamepadAction::NavigateRight: return "navigate_right";
                case GamepadAction::Select: return "select";
                case GamepadAction::Deselect: return "deselect";
                case GamepadAction::ToggleSelect: return "toggle_select";
                case GamepadAction::Confirm: return "confirm";
                case GamepadAction::Cancel: return "cancel";
                case GamepadAction::Undo: return "undo";
                case GamepadAction::Redo: return "redo";
                case GamepadAction::Compile: return "compile";
                case GamepadAction::Copy: return "copy";
                case GamepadAction::Paste: return "paste";
                case GamepadAction::Delete: return "delete";
                case GamepadAction::AutoLayout: return "auto_layout";
                case GamepadAction::CreateNode: return "create_node";
                case GamepadAction::ExtractFunction: return "extract_function";
                case GamepadAction::CenterView: return "center_view";
                case GamepadAction::OpenMenu: return "open_menu";
                case GamepadAction::NavigateBack: return "navigate_back";
                case GamepadAction::NavigateForward: return "navigate_forward";
                case GamepadAction::Count: return "count";
            }
            return "unknown";
        }

        std::string buttonToConfigString(GamepadButton button)
        {
            switch (button)
            {
                case GamepadButton::A: return "A";
                case GamepadButton::B: return "B";
                case GamepadButton::X: return "X";
                case GamepadButton::Y: return "Y";
                case GamepadButton::LB: return "LB";
                case GamepadButton::RB: return "RB";
                case GamepadButton::LStick: return "LSTICK";
                case GamepadButton::RStick: return "RSTICK";
                case GamepadButton::Back: return "BACK";
                case GamepadButton::Forward: return "FORWARD";
                case GamepadButton::DPadUp: return "DPAD_UP";
                case GamepadButton::DPadDown: return "DPAD_DOWN";
                case GamepadButton::DPadLeft: return "DPAD_LEFT";
                case GamepadButton::DPadRight: return "DPAD_RIGHT";
                case GamepadButton::LT: return "LT";
                case GamepadButton::RT: return "RT";
                case GamepadButton::Count: return "Count";
            }
            return "Unknown";
        }

        GamepadButton configStringToButton(const std::string & str)
        {
            if (str == "A") return GamepadButton::A;
            if (str == "B") return GamepadButton::B;
            if (str == "X") return GamepadButton::X;
            if (str == "Y") return GamepadButton::Y;
            if (str == "LB") return GamepadButton::LB;
            if (str == "RB") return GamepadButton::RB;
            if (str == "LSTICK") return GamepadButton::LStick;
            if (str == "RSTICK") return GamepadButton::RStick;
            if (str == "DPAD_UP") return GamepadButton::DPadUp;
            if (str == "DPAD_DOWN") return GamepadButton::DPadDown;
            if (str == "DPAD_LEFT") return GamepadButton::DPadLeft;
            if (str == "DPAD_RIGHT") return GamepadButton::DPadRight;
            if (str == "LT") return GamepadButton::LT;
            if (str == "RT") return GamepadButton::RT;
            if (str == "BACK") return GamepadButton::Back;
            if (str == "FORWARD") return GamepadButton::Forward;
            return GamepadButton::Count;
        }

        // Preset profiles for different controller layouts
        struct PresetProfile {
            std::string name;
            std::unordered_map<GamepadAction, GamepadButton> bindings;
        };

        PresetProfile createXboxPreset()
        {
            PresetProfile profile;
            profile.name = "Xbox";
            auto & map = profile.bindings;
            map[GamepadAction::NavigateUp] = GamepadButton::DPadUp;
            map[GamepadAction::NavigateDown] = GamepadButton::DPadDown;
            map[GamepadAction::NavigateLeft] = GamepadButton::DPadLeft;
            map[GamepadAction::NavigateRight] = GamepadButton::DPadRight;
            map[GamepadAction::Select] = GamepadButton::A;
            map[GamepadAction::Deselect] = GamepadButton::B;
            map[GamepadAction::ToggleSelect] = GamepadButton::X;
            map[GamepadAction::Confirm] = GamepadButton::A;
            map[GamepadAction::Cancel] = GamepadButton::B;
            map[GamepadAction::OpenMenu] = GamepadButton::Y;
            map[GamepadAction::Undo] = GamepadButton::LB;
            map[GamepadAction::Redo] = GamepadButton::RB;
            map[GamepadAction::Compile] = GamepadButton::LB;
            map[GamepadAction::Delete] = GamepadButton::RB;
            return profile;
        }

        PresetProfile createPlayStationPreset()
        {
            PresetProfile profile;
            profile.name = "PlayStation";
            auto & map = profile.bindings;
            map[GamepadAction::NavigateUp] = GamepadButton::DPadUp;
            map[GamepadAction::NavigateDown] = GamepadButton::DPadDown;
            map[GamepadAction::NavigateLeft] = GamepadButton::DPadLeft;
            map[GamepadAction::NavigateRight] = GamepadButton::DPadRight;
            map[GamepadAction::Select] = GamepadButton::A;
            map[GamepadAction::Deselect] = GamepadButton::B;
            map[GamepadAction::ToggleSelect] = GamepadButton::X;
            map[GamepadAction::Confirm] = GamepadButton::A;
            map[GamepadAction::Cancel] = GamepadButton::B;
            map[GamepadAction::OpenMenu] = GamepadButton::Y;
            map[GamepadAction::Undo] = GamepadButton::LB;
            map[GamepadAction::Redo] = GamepadButton::RB;
            return profile;
        }

        PresetProfile createGenericPreset()
        {
            PresetProfile profile;
            profile.name = "Generic";
            auto & map = profile.bindings;
            map[GamepadAction::Select] = GamepadButton::A;
            map[GamepadAction::Deselect] = GamepadButton::B;
            map[GamepadAction::ToggleSelect] = GamepadButton::X;
            map[GamepadAction::Confirm] = GamepadButton::A;
            map[GamepadAction::Cancel] = GamepadButton::B;
            map[GamepadAction::OpenMenu] = GamepadButton::Y;
            return profile;
        }

        std::vector<PresetProfile> getPresetProfiles()
        {
            return { createXboxPreset(), createPlayStationPreset(), createGenericPreset() };
        }
    } // namespace

    void GamepadSettingsDialog::show()
    {
        m_visible = true;
    }

    void GamepadSettingsDialog::hide()
    {
        m_visible = false;
        m_isCapturingInput = false;
    }

    [[nodiscard]] bool GamepadSettingsDialog::isVisible() const
    {
        return m_visible;
    }

    void GamepadSettingsDialog::render()
    {
        if (!m_visible)
        {
            return;
        }

        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_AlwaysAutoResize;

        if (!ImGui::Begin("Gamepad Settings", &m_visible, windowFlags))
        {
            ImGui::End();
            return;
        }

        renderGamepadBindingsPanel(m_searchFilter, m_isCapturingInput, m_capturingAction);

        ImGui::End();
    }

    void GamepadSettingsDialog::resetToDefaults()
    {
        GamepadActionMap::instance().resetToDefaults();
    }

    void GamepadSettingsDialog::saveToFile(const std::string & filepath) const
    {
        nlohmann::json json;
        auto & actionMap = GamepadActionMap::instance();

        auto actions = actionMap.getAllActions();
        for (const auto & [action, /*name*/bindingName] : actions)
        {
            if (action == GamepadAction::Count)
            {
                continue;
            }
            std::string bindingStr = actionMap.getBindingDisplay(action);
            json[actionToJsonKey(action)] = bindingStr;
        }

        std::ofstream file(filepath);
        if (file.is_open())
        {
            file << json.dump(2);
            file.close();
        }
    }

    void GamepadSettingsDialog::loadFromFile(const std::string & filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            return;
        }

        nlohmann::json json;
        file >> json;

        auto & actionMap = GamepadActionMap::instance();

        for (auto && [key, value] : json.items())
        {
            std::string const bindingStr = value.get<std::string>();
            GamepadButton const button = configStringToButton(bindingStr);
            if (button == GamepadButton::Count)
            {
                continue;
            }

            // Find the action by JSON key name using getAllActions() loop
            GamepadAction action = GamepadAction::Count;
            for (auto const & [act, actionName] : actionMap.getAllActions())
            {
                if (actionToJsonKey(act) == key)
                {
                    action = act;
                    break;
                }
            }
            if (action != GamepadAction::Count)
            {
                actionMap.remapAction(action, button);
            }
        }
    }

    void GamepadSettingsDialog::loadPreset(const std::string & profile)
    {
        auto presets = getPresetProfiles();
        for (const auto & preset : presets)
        {
            if (preset.name == profile)
            {
                auto & actionMap = GamepadActionMap::instance();
                actionMap.resetToDefaults(); // Reset first, then apply preset
                for (const auto & [action, button] : preset.bindings)
                {
                    actionMap.remapAction(action, button);
                }
                break;
            }
        }
    }

    void GamepadSettingsDialog::startRecording(GamepadAction action)
    {
        m_isCapturingInput = true;
        m_capturingAction = action;
    }

    void GamepadSettingsDialog::renderBindingList()
    {
        auto & actionMap = GamepadActionMap::instance();
        auto actions = actionMap.getAllActions();

        // Filter actions based on search
        std::vector<std::pair<GamepadAction, std::string>> filteredActions;
        for (const auto & [action, name] : actions)
        {
            if (m_searchFilter.empty() || name.find(m_searchFilter) != std::string::npos)
            {
                filteredActions.push_back({action, name});
            }
        }

        // Simple list display
        for (int i = 0; i < static_cast<int>(filteredActions.size()); ++i)
        {
            renderBindingRow(filteredActions[i].first, i);
        }
    }

    void GamepadSettingsDialog::renderBindingRow(GamepadAction action, int index)
    {
        auto & actionMap = GamepadActionMap::instance();
        const std::string & actionName = actionMap.getActionName(action);
        std::string bindingDisplay = actionMap.getBindingDisplay(action);

        ImGui::PushID(index);

        // Action name
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%-25s", actionName.c_str());
        ImGui::SameLine();

        // Binding display
        if (m_isCapturingInput && m_capturingAction == action)
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "< Press a button... >");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", bindingDisplay.c_str());
        }

        ImGui::SameLine();

        // Remap button
        if (ImGui::Button("Remap"))
        {
            startRecording(action);
        }

        ImGui::NextColumn();
    }

    // ---------------------------------------------------------------------------
    // Shared panel — can be embedded in any ImGui window
    // ---------------------------------------------------------------------------

    void renderGamepadBindingsPanel(std::string & searchFilter,
                                    bool & isCapturingInput,
                                    GamepadAction & capturingAction)
    {
        auto & actionMap = GamepadActionMap::instance();

        // --- Search filter ---
        ImGui::Text("Filter:");
        ImGui::SameLine();
        ImGui::InputText("##GamepadSearch", &searchFilter);
        ImGui::SameLine();
        if (ImGui::Button("Clear##GPFilter"))
        {
            searchFilter.clear();
        }

        ImGui::Separator();

        // --- Preset profiles ---
        ImGui::Text("Preset:");
        ImGui::SameLine();
        auto presets = getPresetProfiles();
        for (auto const & preset : presets)
        {
            if (ImGui::Button(preset.name.c_str()))
            {
                actionMap.resetToDefaults();
                for (auto const & [action, button] : preset.bindings)
                {
                    actionMap.remapAction(action, button);
                }
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        if (ImGui::Button("Reset All to Defaults"))
        {
            actionMap.resetToDefaults();
        }

        ImGui::Separator();

        // --- Capture state indicator ---
        if (isCapturingInput)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 0, 255));
            ImGui::TextUnformatted("Press a button on the gamepad...");
            ImGui::TextUnformatted("Press Escape to cancel");
            ImGui::PopStyleColor();

            // Allow cancelling with Escape
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                isCapturingInput = false;
                capturingAction = GamepadAction::Count;
            }

            // Detect a gamepad button press and apply remap
            for (int i = 0; i < static_cast<int>(GamepadButton::Count); ++i)
            {
                auto btn = static_cast<GamepadButton>(i);
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_GamepadFaceDown + i), false))
                {
                    actionMap.remapAction(capturingAction, btn);
                    isCapturingInput = false;
                    capturingAction = GamepadAction::Count;
                    break;
                }
            }
        }

        ImGui::Separator();

        // --- Binding table ---
        auto allActions = actionMap.getAllActions();

        if (ImGui::BeginTable("GPBindings", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Button",  ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Remap",   ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            int row = 0;
            for (auto const & [action, name] : allActions)
            {
                if (action == GamepadAction::Count)
                {
                    continue;
                }

                // Apply search filter
                if (!searchFilter.empty())
                {
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    std::string filterLower = searchFilter;
                    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower.find(filterLower) == std::string::npos)
                    {
                        continue;
                    }
                }

                ImGui::TableNextRow();
                ImGui::PushID(row++);

                // Action name
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());

                // Current binding
                ImGui::TableNextColumn();
                if (isCapturingInput && capturingAction == action)
                {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "< Press button... >");
                }
                else
                {
                    ImGui::TextUnformatted(actionMap.getBindingDisplay(action).c_str());
                }

                // Remap button
                ImGui::TableNextColumn();
                if (ImGui::Button("Remap"))
                {
                    isCapturingInput = true;
                    capturingAction = action;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    }

} // namespace gladius::ui
