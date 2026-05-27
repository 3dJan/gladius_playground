#pragma once

#include "GamepadState.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Gamepad actions that map to editor operations.
     *
     * These actions are higher-level than raw buttons - they represent
     * what the user wants to do rather than which physical button they press.
     */
    enum class GamepadAction {
        // Navigation
        NavigateUp,
        NavigateDown,
        NavigateLeft,
        NavigateRight,

        // Selection
        Select,           // A/X button - select node
        Deselect,         // B/Circle - deselect or go back
        ToggleSelect,     // X/Square - toggle selection

        // Actions
        Confirm,          // Same as Select for menu confirmation
        Cancel,           // Same as Deselect for closing menus

        // Editor actions
        Undo,
        Redo,
        Compile,
        Copy,
        Paste,
        Delete,
        AutoLayout,
        CreateNode,
        ExtractFunction,
        CenterView,

        // Menu navigation
        OpenMenu,         // Y button - open context menu
        NavigateBack,     // Navigate to previous function
        NavigateForward,  // Navigate to next function

        Count
    };

    /**
     * @brief Maps GamepadAction to default button combinations.
     *
     * Provides default Xbox-style bindings and allows runtime remapping.
     * Single buttons for navigation, combos (shoulder + face) for editor actions.
     */
    class GamepadActionMap {
      public:
        static GamepadActionMap & instance();

        // Non-copyable
        GamepadActionMap(GamepadActionMap const &) = delete;
        GamepadActionMap & operator=(GamepadActionMap const &) = delete;

        /**
         * @brief Get the primary button(s) for an action.
         * @param action The gamepad action
         * @return The primary button for single-button actions
         */
        [[nodiscard]] GamepadButton getPrimaryButton(GamepadAction action) const;

        /**
         * @brief Check if an action is pressed (single button or combo).
         * @param gamepad The current gamepad state
         * @param action The action to check
         * @return true if the action is currently triggered
         */
        [[nodiscard]] bool isActionPressed(GamepadState const & gamepad, GamepadAction action) const;

        /**
         * @brief Check if an action is held (button held down).
         * @param gamepad The current gamepad state
         * @param action The action to check
         * @return true if the action is currently held
         */
        [[nodiscard]] bool isActionHeld(GamepadState const & gamepad, GamepadAction action) const;

        /**
         * @brief Get a human-readable name for an action.
         * @param action The action
         * @return Display name string
         */
        [[nodiscard]] std::string getActionName(GamepadAction action) const;

        /**
         * @brief Get the display text for an action's binding.
         * @param action The action
         * @return String like "A", "LB + A", etc.
         */
        [[nodiscard]] std::string getBindingDisplay(GamepadAction action) const;

        /**
         * @brief Remap an action to a different button.
         * @param action The action to remap
         * @param button The new primary button
         */
        void remapAction(GamepadAction action, GamepadButton button);

        /**
         * @brief Reset all bindings to defaults.
         */
        void resetToDefaults();

        /**
         * @brief Check if a combo action is pressed (shoulder + face button).
         * @param gamepad The current gamepad state
         * @param shoulder The shoulder button (LB or RB)
         * @param face The face button (A, B, X, or Y)
         * @return true if both buttons are pressed
         */
        [[nodiscard]] static bool isComboPressed(GamepadState const & gamepad,
                                                  GamepadButton shoulder,
                                                  GamepadButton face);

        /**
         * @brief Get all action names for UI display.
         * @return Vector of (action, display_name) pairs
         */
        std::vector<std::pair<GamepadAction, std::string>> getAllActions() const;

      private:
        GamepadActionMap();  ///< Initialises with default key bindings.
        ~GamepadActionMap() = default;

        // Default binding table: action -> primary button
        std::unordered_map<GamepadAction, GamepadButton> m_bindings;

        // Combo bindings: action -> (shoulder, face) pair
        struct ComboBinding
        {
            GamepadButton shoulder;
            GamepadButton face;
            bool active;
        };
        std::unordered_map<GamepadAction, ComboBinding> m_comboBindings;

        void initDefaultBindings();
    };

    /**
     * @brief Utility function to convert GamepadButton to string.
     */
    [[nodiscard]] std::string buttonToString(GamepadButton button);

    /**
     * @brief Utility function to convert string to GamepadButton.
     */
    [[nodiscard]] GamepadButton stringToButton(std::string const & str);

} // namespace gladius::ui
