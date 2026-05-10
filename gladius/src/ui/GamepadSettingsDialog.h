#pragma once

#include "GamepadActionMap.h"
#include "GamepadState.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace gladius::ui
{
    /**
     * @brief Dialog for configuring gamepad bindings
     *
     * Allows users to view and customize gamepad bindings for various editor actions.
     * Supports remapping single-button actions and combo bindings.
     */
    class GamepadSettingsDialog
    {
      public:
        /**
         * @brief Construct a new GamepadSettingsDialog
         */
        GamepadSettingsDialog() = default;

        /**
         * @brief Destroy the GamepadSettingsDialog
         */
        ~GamepadSettingsDialog() = default;

        /**
         * @brief Show the gamepad settings dialog
         */
        void show();

        /**
         * @brief Hide the gamepad settings dialog
         */
        void hide();

        /**
         * @brief Check if the dialog is currently visible
         * @return true if the dialog is visible
         */
        [[nodiscard]] bool isVisible() const;

        /**
         * @brief Render the dialog
         *
         * This should be called every frame if the dialog is visible.
         */
        void render();

        /**
         * @brief Reset all bindings to defaults
         */
        void resetToDefaults();

        /**
         * @brief Save current bindings to a file
         * @param filepath Path to save the configuration
         */
        void saveToFile(const std::string & filepath) const;

        /**
         * @brief Load bindings from a file
         * @param filepath Path to load the configuration from
         */
        void loadFromFile(const std::string & filepath);

        /**
         * @brief Load a preset profile
         * @param profile The preset profile name (e.g., "Xbox", "PlayStation", "Generic")
         */
        void loadPreset(const std::string & profile);

      private:
        /**
         * @brief Start recording a button press for an action
         * @param action The action to remap
         */
        void startRecording(GamepadAction action);

        /**
         * @brief Render the binding editor for all actions
         */
        void renderBindingList();

        /**
         * @brief Render a single binding row
         * @param action The action to render
         * @param index The index of the action in the list
         */
        void renderBindingRow(GamepadAction action, int index);

        std::string m_searchFilter;
        bool m_visible = false;

        // For capturing gamepad input
        bool m_isCapturingInput = false;
        GamepadAction m_capturingAction = GamepadAction::Count;

        // Disable copy and move
        GamepadSettingsDialog(GamepadSettingsDialog const &) = delete;
        GamepadSettingsDialog & operator=(GamepadSettingsDialog const &) = delete;
        GamepadSettingsDialog(GamepadSettingsDialog &&) = delete;
        GamepadSettingsDialog & operator=(GamepadSettingsDialog &&) = delete;
    };

    /**
     * @brief Render the gamepad bindings editor panel inline.
     *
     * This is a standalone function so the same panel can be embedded into
     * other dialogs (e.g. the Input Settings dialog) without duplicating logic.
     * The @p isCapturingInput and @p capturingAction parameters carry the
     * capture state that the caller must persist across frames.
     *
     * @param searchFilter    Filter string (modified in-place)
     * @param isCapturingInput Whether a button remap is in progress (modified)
     * @param capturingAction  Action being remapped (modified)
     */
    void renderGamepadBindingsPanel(std::string & searchFilter,
                                    bool & isCapturingInput,
                                    GamepadAction & capturingAction);

} // namespace gladius::ui
