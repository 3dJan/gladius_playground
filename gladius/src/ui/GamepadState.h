#pragma once

#include <Eigen/Core>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace gladius::ui
{
    /**
     * @brief Gamepad button enumeration covering Xbox, PlayStation, and generic layouts.
     */
    enum class GamepadButton {
        A,          // Xbox A / PlayStation Cross (bottom)
        B,          // Xbox B / PlayStation Circle (right)
        X,          // Xbox X / PlayStation Square (left)
        Y,          // Xbox Y / PlayStation Triangle (top)
        LB,         // Left bumper
        RB,         // Right bumper
        LStick,     // Left stick click
        RStick,     // Right stick click
        Back,       // Xbox "Back" / PlayStation "Share"
        Forward,    // Xbox "Start" / PlayStation "Options"
        DPadUp,     // D-pad up
        DPadDown,   // D-pad down
        DPadLeft,   // D-pad left
        DPadRight,  // D-pad right
        LT,         // Left trigger (analog)
        RT,         // Right trigger (analog)
        Count       // Sentinel value
    };

    /**
     * @brief Information about a connected gamepad.
     */
    struct GamepadInfo
    {
        int instanceId{0};    ///< GLFW joystick instance ID
        std::string name;     ///< Human-readable gamepad name
        bool connected{false}; ///< Whether the gamepad is currently connected
    };

    /**
     * @brief Singleton that tracks gamepad input state via ImGui's built-in gamepad support.
     *
     * This class reads from ImGui's io.NavInputs[] and input events, which are
     * automatically populated by the GLFW backend when ImGuiConfigFlags_NavEnableGamepad
     * is enabled. No direct GLFW polling is needed.
     *
     * Usage:
     *   GamepadState::instance().update();  // call once per frame
     *   if (GamepadState::instance().isButtonPressed(GamepadButton::A)) { ... }
     */
    class GamepadState {
      public:
        static GamepadState & instance();

        // Non-copyable
        GamepadState(GamepadState const &) = delete;
        GamepadState & operator=(GamepadState const &) = delete;

        /**
         * @brief Update internal state from ImGui's gamepad input.
         *        Must be called once per frame before querying button states.
         */
        void update();

        // --- Connection state ---

        /** @brief Get a list of all connected gamepads. */
        [[nodiscard]] std::vector<GamepadInfo> connectedGamepads() const;

        /** @brief True if at least one gamepad is connected via ImGui. */
        [[nodiscard]] bool isAnyConnected() const;

        /** @brief True if gamepad input is active (connected and ImGui has gamepad flag). */
        [[nodiscard]] bool isActive() const;

        // --- Button queries (frame-level detection) ---

        /**
         * @brief True if the button was pressed this frame (transition from up to down).
         */
        [[nodiscard]] bool isButtonPressed(GamepadButton button) const;

        /**
         * @brief True if the button was released this frame (transition from down to up).
         */
        [[nodiscard]] bool isButtonReleased(GamepadButton button) const;

        /**
         * @brief True if the button is currently held down.
         */
        [[nodiscard]] bool isButtonHeld(GamepadButton button) const;

        /**
         * @brief Set the held state of a button (used internally by update()).
         */
        void setButtonHeld(GamepadButton button, bool held);

        /**
         * @brief True if the button has been held for at least holdThreshold seconds.
         *        Useful for repeatable actions (e.g., hold to delete).
         */
        [[nodiscard]] bool isButtonHeldFor(GamepadButton button, float holdThreshold) const;

        // --- Analog sticks ---

        /**
         * @brief Get left stick axis as a normalized 2D vector (-1 to 1).
         *        Values below the deadzone threshold are clamped to zero.
         */
        [[nodiscard]] Eigen::Vector2f getLeftStick() const;

        /**
         * @brief Get right stick axis as a normalized 2D vector (-1 to 1).
         *        Values below the deadzone threshold are clamped to zero.
         */
        [[nodiscard]] Eigen::Vector2f getRightStick() const;

        /**
         * @brief True if the left stick has any significant movement (above deadzone).
         */
        [[nodiscard]] bool isLeftStickActive() const;

        /**
         * @brief True if the right stick has any significant movement (above deadzone).
         */
        [[nodiscard]] bool isRightStickActive() const;

        // --- Triggers ---

        /**
         * @brief Get left trigger value (0.0 to 1.0).
         *        Triggers are not deadzone-clamped so users can detect partial presses.
         */
        [[nodiscard]] float getLeftTrigger() const;

        /**
         * @brief Get right trigger value (0.0 to 1.0).
         */
        [[nodiscard]] float getRightTrigger() const;

        // --- Configuration ---

        /** @brief Get the stick deadzone threshold (default: 0.25). */
        [[nodiscard]] float stickDeadzone() const { return m_stickDeadzone; }

        /** @brief Set the stick deadzone threshold. */
        void setStickDeadzone(float deadzone) { m_stickDeadzone = deadzone; }

        /** @brief Get the trigger deadzone threshold (default: 0.1). */
        [[nodiscard]] float triggerDeadzone() const { return m_triggerDeadzone; }

        /** @brief Set the trigger deadzone threshold. */
        void setTriggerDeadzone(float deadzone) { m_triggerDeadzone = deadzone; }

        /**
         * @brief Reset all internal state (used when disconnecting a gamepad).
         */
        void reset();

      private:
        GamepadState() = default;
        ~GamepadState() = default;

        // Frame-level button events (cleared each frame)
        std::unordered_set<GamepadButton> m_buttonsPressed;
        std::unordered_set<GamepadButton> m_buttonsReleased;

        // Current held state of buttons
        std::unordered_map<GamepadButton, bool> m_heldButtons;

        // Current analog values (from ImGui NavInputs).
        Eigen::Vector2f m_leftStick{0.0f, 0.0f};
        Eigen::Vector2f m_rightStick{0.0f, 0.0f};
        float m_leftTrigger{0.0f};
        float m_rightTrigger{0.0f};

        // Configuration.
        float m_stickDeadzone{0.25f};
        float m_triggerDeadzone{0.1f};

        // Connection state (from ImGui backend flags).
        bool m_isAnyConnected{false};
    };

} // namespace gladius::ui
