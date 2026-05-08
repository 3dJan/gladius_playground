#include "GamepadState.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

namespace gladius::ui {

GamepadState & GamepadState::instance() {
    static GamepadState state;
    return state;
}

void GamepadState::update() {
    // ImGui processes gamepad input automatically when ImGuiConfigFlags_NavEnableGamepad is set.
    // We read from ImGui's io.NavInputs[] to get analog stick values.
    // Note: The newer ImGui versions don't expose detailed input events via InputQueueEvents,
    // so we rely on button polling and analog values instead.
    
    ImGuiIO & io = ImGui::GetIO();
    
    // Reset pressed/released tracking (these are frame-only events)
    m_buttonsReleased.clear();
    m_buttonsPressed.clear();
    
    // Read analog values from io.NavInputs (float range 0.0 to 1.0)
    // These are set by the backend when gamepad axes move past a deadzone threshold
    // Left stick X/Y (stored in NavInputs[ImGuiNavInput_LStickLeft] and ImGuiNavInput_LStickUp)
    m_leftStick.x() = io.NavInputs[ImGuiNavInput_LStickLeft];  // Negative = left, Positive = right
    m_leftStick.y() = io.NavInputs[ImGuiNavInput_LStickUp];    // Negative = down, Positive = up
    
    // Right stick - ImGui backend doesn't auto-populate right stick into NavInputs[]
    // Right stick values would need to be read from io.MouseWheelY or custom GLFW callbacks
    // For now, initialize to zero and update via direct axis reading if needed
    m_rightStick.x() = 0.0f;
    m_rightStick.y() = 0.0f;
    
    // Check if any gamepad is connected via ImGui
    m_isAnyConnected = (io.BackendFlags & ImGuiBackendFlags_HasGamepad);
}

[[nodiscard]] bool GamepadState::isButtonHeld(GamepadButton button) const {
    auto it = m_heldButtons.find(button);
    return it != m_heldButtons.end() && it->second;
}

void GamepadState::setButtonHeld(GamepadButton button, bool held) {
    m_heldButtons[button] = held;
}

[[nodiscard]] bool GamepadState::isButtonHeldFor(GamepadButton button, float /*holdThreshold*/) const {
    auto it = m_heldButtons.find(button);
    if (it == m_heldButtons.end() || !it->second) {
        return false;
    }
    // TODO: Add timing logic to check if held for the specified duration
    return true; // Simplified - would need frame counting for accurate timing
}

[[nodiscard]] Eigen::Vector2f GamepadState::getLeftStick() const {
    return m_leftStick;
}

[[nodiscard]] Eigen::Vector2f GamepadState::getRightStick() const {
    return m_rightStick;
}

[[nodiscard]] bool GamepadState::isLeftStickActive() const {
    float magnitude = m_leftStick.squaredNorm();
    return magnitude > (m_stickDeadzone * m_stickDeadzone);
}

[[nodiscard]] bool GamepadState::isRightStickActive() const {
    float magnitude = m_rightStick.squaredNorm();
    return magnitude > (m_stickDeadzone * m_stickDeadzone);
}

[[nodiscard]] float GamepadState::getLeftTrigger() const {
    return m_leftTrigger;
}

[[nodiscard]] float GamepadState::getRightTrigger() const {
    return m_rightTrigger;
}

[[nodiscard]] std::vector<GamepadInfo> GamepadState::connectedGamepads() const {
    // ImGui doesn't expose individual gamepad info directly.
    // We'd need to query GLFW for connected gamepads if detailed info is needed.
    // For now, return empty list (state is context-aware - only active when editor is visible)
    return {};
}

[[nodiscard]] bool GamepadState::isAnyConnected() const {
    return m_isAnyConnected;
}

[[nodiscard]] bool GamepadState::isActive() const {
    return m_isAnyConnected && (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad);
}

[[nodiscard]] bool GamepadState::isButtonPressed(GamepadButton button) const {
    auto it = m_buttonsPressed.find(button);
    return it != m_buttonsPressed.end();
}

[[nodiscard]] bool GamepadState::isButtonReleased(GamepadButton button) const {
    auto it = m_buttonsReleased.find(button);
    return it != m_buttonsReleased.end();
}

void GamepadState::reset() {
    m_buttonsPressed.clear();
    m_buttonsReleased.clear();
    m_heldButtons.clear();
    m_leftStick.setZero();
    m_rightStick.setZero();
    m_leftTrigger = 0.0f;
    m_rightTrigger = 0.0f;
    m_isAnyConnected = false;
}

} // namespace gladius::ui
