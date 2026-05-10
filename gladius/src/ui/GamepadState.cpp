#include "GamepadState.h"

#include <imgui.h>

namespace gladius::ui
{

GamepadState & GamepadState::instance()
{
    static GamepadState state;
    return state;
}

void GamepadState::update()
{
    ImGuiIO & io = ImGui::GetIO();

    m_isAnyConnected = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;

    // Button → ImGuiKey mapping
    struct ButtonKeyMapping
    {
        GamepadButton button;
        ImGuiKey key;
    };

    constexpr ButtonKeyMapping mappings[] = {
        {GamepadButton::A,         ImGuiKey_GamepadFaceDown  },
        {GamepadButton::B,         ImGuiKey_GamepadFaceRight },
        {GamepadButton::X,         ImGuiKey_GamepadFaceLeft  },
        {GamepadButton::Y,         ImGuiKey_GamepadFaceUp    },
        {GamepadButton::LB,        ImGuiKey_GamepadL1        },
        {GamepadButton::RB,        ImGuiKey_GamepadR1        },
        {GamepadButton::LStick,    ImGuiKey_GamepadL3        },
        {GamepadButton::RStick,    ImGuiKey_GamepadR3        },
        {GamepadButton::Back,      ImGuiKey_GamepadBack      },
        {GamepadButton::Forward,   ImGuiKey_GamepadStart     },
        {GamepadButton::DPadUp,    ImGuiKey_GamepadDpadUp    },
        {GamepadButton::DPadDown,  ImGuiKey_GamepadDpadDown  },
        {GamepadButton::DPadLeft,  ImGuiKey_GamepadDpadLeft  },
        {GamepadButton::DPadRight, ImGuiKey_GamepadDpadRight },
        {GamepadButton::LT,        ImGuiKey_GamepadL2        },
        {GamepadButton::RT,        ImGuiKey_GamepadR2        },
    };

    m_buttonsPressed.clear();
    m_buttonsReleased.clear();

    for (auto const & mapping : mappings)
    {
        bool const nowDown = ImGui::IsKeyDown(mapping.key);
        bool const wasHeld = isButtonHeld(mapping.button);

        if (nowDown && !wasHeld)
        {
            m_buttonsPressed.insert(mapping.button);
        }
        else if (!nowDown && wasHeld)
        {
            m_buttonsReleased.insert(mapping.button);
        }

        m_heldButtons[mapping.button] = nowDown;
    }

    // Read analog stick values via io.KeysData
    auto getAnalog = [&io](ImGuiKey key) -> float
    {
        int const idx = static_cast<int>(key) - ImGuiKey_NamedKey_BEGIN;
        return io.KeysData[idx].AnalogValue;
    };

    m_leftStick.x()  =  getAnalog(ImGuiKey_GamepadLStickRight) - getAnalog(ImGuiKey_GamepadLStickLeft);
    m_leftStick.y()  =  getAnalog(ImGuiKey_GamepadLStickUp)    - getAnalog(ImGuiKey_GamepadLStickDown);
    m_rightStick.x() =  getAnalog(ImGuiKey_GamepadRStickRight) - getAnalog(ImGuiKey_GamepadRStickLeft);
    m_rightStick.y() =  getAnalog(ImGuiKey_GamepadRStickUp)    - getAnalog(ImGuiKey_GamepadRStickDown);

    m_leftTrigger  = getAnalog(ImGuiKey_GamepadL2);
    m_rightTrigger = getAnalog(ImGuiKey_GamepadR2);
}

[[nodiscard]] bool GamepadState::isButtonHeld(GamepadButton button) const
{
    auto it = m_heldButtons.find(button);
    return it != m_heldButtons.end() && it->second;
}

void GamepadState::setButtonHeld(GamepadButton button, bool held)
{
    m_heldButtons[button] = held;
}

[[nodiscard]] bool GamepadState::isButtonHeldFor(GamepadButton button, float /*holdThreshold*/) const
{
    auto it = m_heldButtons.find(button);
    if (it == m_heldButtons.end() || !it->second)
    {
        return false;
    }
    // TODO: Add timing logic to check if held for the specified duration
    return true;
}

[[nodiscard]] Eigen::Vector2f GamepadState::getLeftStick() const
{
    return m_leftStick;
}

[[nodiscard]] Eigen::Vector2f GamepadState::getRightStick() const
{
    return m_rightStick;
}

[[nodiscard]] bool GamepadState::isLeftStickActive() const
{
    return m_leftStick.squaredNorm() > (m_stickDeadzone * m_stickDeadzone);
}

[[nodiscard]] bool GamepadState::isRightStickActive() const
{
    return m_rightStick.squaredNorm() > (m_stickDeadzone * m_stickDeadzone);
}

[[nodiscard]] float GamepadState::getLeftTrigger() const
{
    return m_leftTrigger;
}

[[nodiscard]] float GamepadState::getRightTrigger() const
{
    return m_rightTrigger;
}

[[nodiscard]] std::vector<GamepadInfo> GamepadState::connectedGamepads() const
{
    return {};
}

[[nodiscard]] bool GamepadState::isAnyConnected() const
{
    return m_isAnyConnected;
}

[[nodiscard]] bool GamepadState::isActive() const
{
    return m_isAnyConnected && (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad);
}

[[nodiscard]] bool GamepadState::isButtonPressed(GamepadButton button) const
{
    return m_buttonsPressed.count(button) > 0;
}

[[nodiscard]] bool GamepadState::isButtonReleased(GamepadButton button) const
{
    return m_buttonsReleased.count(button) > 0;
}

void GamepadState::reset()
{
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
