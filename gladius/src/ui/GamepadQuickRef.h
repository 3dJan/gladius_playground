#pragma once

namespace gladius::ui
{

    /**
     * @brief A compact always-auto-resize overlay that shows all gamepad
     *        bindings at a glance.
     *
     * Open from the main menu or by pressing the Start / Forward button on the
     * gamepad while in the model editor.  Tracks its own visibility flag so
     * callers only need to call @c show(), @c hide(), and @c render() each frame.
     */
    class GamepadQuickRef
    {
      public:
        GamepadQuickRef() = default;
        ~GamepadQuickRef() = default;

        GamepadQuickRef(GamepadQuickRef const &) = delete;
        GamepadQuickRef & operator=(GamepadQuickRef const &) = delete;

        /// Make the window visible.
        void show();
        /// Hide the window.
        void hide();
        /// @return True when the window is currently open.
        [[nodiscard]] bool isVisible() const;
        /// Toggle visibility.
        void toggle();

        /**
         * @brief Render the quick reference window.
         *
         * Must be called every frame while ImGui is accepting input/commands.
         * Does nothing when @c isVisible() is false.
         */
        void render();

      private:
        bool m_visible{false};
    };

} // namespace gladius::ui
