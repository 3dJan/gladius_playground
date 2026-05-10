#pragma once

#include "GamepadActionMap.h"
#include "GamepadState.h"

#include <imgui.h>

namespace gladius::ui
{

    /**
     * @brief Renders a context-sensitive button hint bar at the bottom of the
     *        node-editor viewport.
     *
     * The bar is only visible when a gamepad is connected. It shows the most
     * important bindings drawn directly from @c GamepadActionMap::instance() so
     * that remapped buttons are always reflected automatically.
     */
    class GamepadHintBar
    {
      public:
        GamepadHintBar() = default;
        ~GamepadHintBar() = default;

        GamepadHintBar(GamepadHintBar const &) = delete;
        GamepadHintBar & operator=(GamepadHintBar const &) = delete;

        /**
         * @brief Render the hint bar anchored to the bottom of the current window.
         *
         * Call this while a node-editor window is active (between ImGui::Begin /
         * ImGui::End) so that @c ImGui::GetWindowDrawList() targets the right
         * layer.  The bar is a no-op when no gamepad is connected.
         */
        void render() const;

      private:
        float m_barHeight{28.0f};      ///< Height of the hint bar in pixels
        float m_paddingX{12.0f};       ///< Horizontal padding inside the bar
        float m_paddingY{6.0f};        ///< Vertical padding inside the bar
        ImU32 m_backgroundColor{IM_COL32(20, 20, 20, 200)};
        ImU32 m_labelColor{IM_COL32(255, 255, 255, 220)};
        ImU32 m_keyColor{IM_COL32(220, 220, 60, 255)};
    };

} // namespace gladius::ui
