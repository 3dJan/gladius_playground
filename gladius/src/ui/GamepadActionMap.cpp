#include "GamepadActionMap.h"

#include <unordered_map>

namespace gladius::ui {

// ============================================================================
// GamepadButton string utilities
// ============================================================================

std::string buttonToString(GamepadButton button) {
    switch (button) {
        case GamepadButton::A:          return "A";
        case GamepadButton::B:          return "B";
        case GamepadButton::X:          return "X";
        case GamepadButton::Y:          return "Y";
        case GamepadButton::LB:         return "LB";
        case GamepadButton::RB:         return "RB";
        case GamepadButton::LStick:     return "LStick";
        case GamepadButton::RStick:     return "RStick";
        case GamepadButton::Back:       return "Back";
        case GamepadButton::Forward:    return "Forward";
        case GamepadButton::DPadUp:     return "DPad Up";
        case GamepadButton::DPadDown:   return "DPad Down";
        case GamepadButton::DPadLeft:   return "DPad Left";
        case GamepadButton::DPadRight:  return "DPad Right";
        case GamepadButton::LT:         return "LT";
        case GamepadButton::RT:         return "RT";
        case GamepadButton::Count:      return "Count";
        default:                        return "Unknown";
    }
}

GamepadButton stringToButton(std::string const & str) {
    if (str == "A")          return GamepadButton::A;
    if (str == "B")          return GamepadButton::B;
    if (str == "X")          return GamepadButton::X;
    if (str == "Y")          return GamepadButton::Y;
    if (str == "LB")         return GamepadButton::LB;
    if (str == "RB")         return GamepadButton::RB;
    if (str == "LStick")     return GamepadButton::LStick;
    if (str == "RStick")     return GamepadButton::RStick;
    if (str == "Back")       return GamepadButton::Back;
    if (str == "Forward")    return GamepadButton::Forward;
    if (str == "DPad Up")    return GamepadButton::DPadUp;
    if (str == "DPad Down")  return GamepadButton::DPadDown;
    if (str == "DPad Left")  return GamepadButton::DPadLeft;
    if (str == "DPad Right") return GamepadButton::DPadRight;
    if (str == "LT")         return GamepadButton::LT;
    if (str == "RT")         return GamepadButton::RT;
    return GamepadButton::Count;
}

// ============================================================================
// GamepadActionMap implementation
// ============================================================================

GamepadActionMap & GamepadActionMap::instance() {
    static GamepadActionMap map;
    return map;
}

GamepadActionMap::GamepadActionMap()
{
    initDefaultBindings();
}

void GamepadActionMap::initDefaultBindings() {
    // Navigation - D-pad
    m_bindings[GamepadAction::NavigateUp]     = GamepadButton::DPadUp;
    m_bindings[GamepadAction::NavigateDown]   = GamepadButton::DPadDown;
    m_bindings[GamepadAction::NavigateLeft]   = GamepadButton::DPadLeft;
    m_bindings[GamepadAction::NavigateRight]  = GamepadButton::DPadRight;

    // Selection
    m_bindings[GamepadAction::Select]         = GamepadButton::A;
    m_bindings[GamepadAction::Deselect]       = GamepadButton::B;
    m_bindings[GamepadAction::ToggleSelect]   = GamepadButton::X;
    m_bindings[GamepadAction::Confirm]        = GamepadButton::A;
    m_bindings[GamepadAction::Cancel]         = GamepadButton::B;

    // Menu
    m_bindings[GamepadAction::OpenMenu]       = GamepadButton::Y;

    // Combo bindings (shoulder + face)
    auto makeCombo = [this](GamepadAction action, GamepadButton shoulder, GamepadButton face) {
        m_comboBindings[action] = {shoulder, face, true};
    };

    makeCombo(GamepadAction::Undo,     GamepadButton::LB, GamepadButton::B);
    makeCombo(GamepadAction::Redo,     GamepadButton::RB, GamepadButton::B);
    makeCombo(GamepadAction::Compile,  GamepadButton::RB, GamepadButton::Y);
    makeCombo(GamepadAction::Copy,     GamepadButton::LB, GamepadButton::X);
    makeCombo(GamepadAction::Paste,    GamepadButton::RB, GamepadButton::X);
    makeCombo(GamepadAction::Delete,   GamepadButton::LB, GamepadButton::B);
    makeCombo(GamepadAction::AutoLayout, GamepadButton::RB, GamepadButton::DPadDown);
    makeCombo(GamepadAction::CreateNode, GamepadButton::LB, GamepadButton::Y);

    // Direct bindings for actions that don't need combos
    m_bindings[GamepadAction::CenterView]     = GamepadButton::RStick;
    m_bindings[GamepadAction::NavigateBack]   = GamepadButton::Back;
    m_bindings[GamepadAction::NavigateForward] = GamepadButton::Forward;
}

[[nodiscard]] GamepadButton GamepadActionMap::getPrimaryButton(GamepadAction action) const {
    auto it = m_bindings.find(action);
    if (it != m_bindings.end()) {
        return it->second;
    }
    return GamepadButton::Count;
}

[[nodiscard]] bool GamepadActionMap::isActionPressed(GamepadState const & gamepad, GamepadAction action) const {
    // Check combo bindings first
    auto comboIt = m_comboBindings.find(action);
    if (comboIt != m_comboBindings.end()) {
        auto const & combo = comboIt->second;
        return isComboPressed(gamepad, combo.shoulder, combo.face);
    }

    // Check single button bindings
    auto btnIt = m_bindings.find(action);
    if (btnIt != m_bindings.end()) {
        return gamepad.isButtonPressed(btnIt->second);
    }

    return false;
}

[[nodiscard]] bool GamepadActionMap::isActionHeld(GamepadState const & gamepad, GamepadAction action) const {
    auto it = m_bindings.find(action);
    if (it != m_bindings.end()) {
        return gamepad.isButtonHeld(it->second);
    }
    return false;
}

std::string GamepadActionMap::getActionName(GamepadAction action) const {
    switch (action) {
        case GamepadAction::NavigateUp:       return "Navigate Up";
        case GamepadAction::NavigateDown:     return "Navigate Down";
        case GamepadAction::NavigateLeft:     return "Navigate Left";
        case GamepadAction::NavigateRight:    return "Navigate Right";
        case GamepadAction::Select:           return "Select";
        case GamepadAction::Deselect:         return "Deselect";
        case GamepadAction::ToggleSelect:     return "Toggle Select";
        case GamepadAction::Confirm:          return "Confirm";
        case GamepadAction::Cancel:           return "Cancel";
        case GamepadAction::Undo:             return "Undo";
        case GamepadAction::Redo:             return "Redo";
        case GamepadAction::Compile:          return "Compile";
        case GamepadAction::Copy:             return "Copy";
        case GamepadAction::Paste:            return "Paste";
        case GamepadAction::Delete:           return "Delete";
        case GamepadAction::AutoLayout:       return "Auto Layout";
        case GamepadAction::CreateNode:       return "Create Node";
        case GamepadAction::ExtractFunction:  return "Extract Function";
        case GamepadAction::CenterView:       return "Center View";
        case GamepadAction::OpenMenu:         return "Open Menu";
        case GamepadAction::NavigateBack:     return "Navigate Back";
        case GamepadAction::NavigateForward:  return "Navigate Forward";
        case GamepadAction::Count:            return "Count";
        default:                              return "Unknown";
    }
}

std::string GamepadActionMap::getBindingDisplay(GamepadAction action) const {
    // Check combo bindings first
    auto comboIt = m_comboBindings.find(action);
    if (comboIt != m_comboBindings.end()) {
        auto const & combo = comboIt->second;
        return buttonToString(combo.shoulder) + " + " + buttonToString(combo.face);
    }

    // Check single button bindings
    auto btnIt = m_bindings.find(action);
    if (btnIt != m_bindings.end()) {
        return buttonToString(btnIt->second);
    }

    return "Not bound";
}

void GamepadActionMap::remapAction(GamepadAction action, GamepadButton button) {
    m_bindings[action] = button;
}

void GamepadActionMap::resetToDefaults() {
    m_bindings.clear();
    m_comboBindings.clear();
    initDefaultBindings();
}

bool GamepadActionMap::isComboPressed(GamepadState const & gamepad,
                                       GamepadButton shoulder,
                                       GamepadButton face) {
    return gamepad.isButtonHeld(shoulder) && gamepad.isButtonPressed(face);
}

std::vector<std::pair<GamepadAction, std::string>> GamepadActionMap::getAllActions() const {
    return {
        {GamepadAction::NavigateUp, "Navigate Up"},
        {GamepadAction::NavigateDown, "Navigate Down"},
        {GamepadAction::NavigateLeft, "Navigate Left"},
        {GamepadAction::NavigateRight, "Navigate Right"},
        {GamepadAction::Select, "Select"},
        {GamepadAction::Deselect, "Deselect"},
        {GamepadAction::ToggleSelect, "Toggle Select"},
        {GamepadAction::Confirm, "Confirm"},
        {GamepadAction::Cancel, "Cancel"},
        {GamepadAction::Undo, "Undo"},
        {GamepadAction::Redo, "Redo"},
        {GamepadAction::Compile, "Compile"},
        {GamepadAction::Copy, "Copy"},
        {GamepadAction::Paste, "Paste"},
        {GamepadAction::Delete, "Delete"},
        {GamepadAction::AutoLayout, "Auto Layout"},
        {GamepadAction::CreateNode, "Create Node"},
        {GamepadAction::ExtractFunction, "Extract Function"},
        {GamepadAction::CenterView, "Center View"},
        {GamepadAction::OpenMenu, "Open Menu"},
        {GamepadAction::NavigateBack, "Navigate Back"},
        {GamepadAction::NavigateForward, "Navigate Forward"},
    };
}

} // namespace gladius::ui
