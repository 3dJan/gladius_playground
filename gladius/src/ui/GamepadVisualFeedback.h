#pragma once

#include <string>
#include <vector>

#include <Eigen/Core>

#include <imgui.h>

namespace nodes
{
    using NodeId = int;
}

namespace gladius::ui
{

    class NodeFocusManager;

    /// @brief Manages visual feedback for gamepad interaction in the node editor.
    /// @details Provides visual indicators like hover rings, toast notifications,
    ///         and context indicators to show gamepad is active and what will be acted upon.
    class GamepadVisualFeedback
    {
      public:
        GamepadVisualFeedback() = default;
        ~GamepadVisualFeedback() = default;

        /// Update the visual feedback state.
        /// @param deltaTime Time since last frame in seconds
        void update(float deltaTime);

        /// Render the node hover ring for the focused node.
        /// @param focusedNode The ID of the currently focused node (0 if none)
        /// @param center The center position of the node in screen space
        /// @param size The size of the node
        /// @param isSelected Whether the node is also selected
        void renderNodeHoverRing(nodes::NodeId focusedNode, ImVec2 center, ImVec2 size, bool isSelected);

        /// Render a toast notification.
        /// @param message The text to display
        /// @param duration How long to show the notification (seconds)
        void showToast(std::string const & message, float duration = 2.0f);

        /// Render the gamepad context indicator (small icon in corner).
        /// @param connected Whether a gamepad is currently connected
        void renderContextIndicator(bool connected) const;

        /// Render menu item highlighting for popup menus.
        /// @param menuItemIndex The index of the currently highlighted menu item
        /// @param itemCount Total number of menu items
        void renderMenuItemHighlight(int menuItemIndex, int itemCount) const;

        /// Check if any visual feedback is currently active.
        /// @return true if there's something to render
        [[nodiscard]] bool isActive() const;

        /// Clear all visual feedback state.
        void clear();

      private:
        /// @brief Toast notification data.
        struct ToastNotification
        {
            std::string message;
            float lifetime;      ///< Current lifetime (seconds)
            float duration;      ///< Total display duration (seconds)
            bool active;         ///< Whether this toast is currently visible
            ImVec4 textColor;    ///< Color for the text
            ImVec2 position;     ///< Screen position
        };

        /// Draw a glowing ring around a rectangle.
        static void drawHoverRing(ImVec2 center, ImVec2 size, float radius, ImVec4 color);

        /// Draw a rounded rectangle outline.
        static void drawRoundedRectOutline(ImVec2 min, ImVec2 max, float rounding, float thickness, ImU32 color);

        /// Render a single toast notification.
        void renderToast(ToastNotification const & toast) const;

        /// Update toast notifications.
        void updateToasts(float deltaTime);

        // State
        std::vector<ToastNotification> m_toasts;
        static constexpr int MAX_TOASTS = 3;

        // Configuration
        float m_ringThickness{3.0f};
        ImVec4 m_ringColor{0.2f, 0.8f, 1.0f, 1.0f};      ///< Cyan glow for focus
        ImVec4 m_ringColorSelected{0.2f, 1.0f, 0.4f, 1.0f}; ///< Green for selected
        float m_ringPulseSpeed{3.0f};                      ///< Pulse speed in Hz
        float m_ringPulseAmount{0.3f};                     ///< Pulse intensity

        ImVec4 m_indicatorColor{0.0f, 0.6f, 0.8f, 0.8f};  ///< Context indicator color
        ImVec2 m_indicatorPosition;                        ///< Position in corner
    };

} // namespace gladius::ui
