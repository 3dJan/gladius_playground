#pragma once

#include <array>
#include <imgui.h>
#include <string>

namespace gladius::ui
{
    /**
     * @brief Draws the Design Agent panel header: title, the design goal and the kebab menu.
     * @details The panel is created with ImGuiWindowFlags_NoTitleBar, so this header also restores
     *          the affordances the native title bar provided: dragging the window and hiding it.
     */
    class AgentPanelHeader
    {
      public:
        /// @brief User intents collected during one header frame; the host owns the behaviour
        struct Commands
        {
            bool hideRequested = false;
            bool settingsRequested = false;
            bool clearSessionRequested = false;
            /// @brief True when goalDraft must be written back as the design goal
            bool goalCommitRequested = false;
            std::string goalDraft;
        };

        /**
         * @brief Renders the header into the current window and advances the layout cursor.
         * @param goal Current one-line design goal; empty while nothing was described yet
         */
        Commands render(std::string const & goal);

        /// @brief Header height for the current font, matching what render() consumes
        [[nodiscard]] static float height();

      private:
        static constexpr std::size_t kGoalCapacity = 192;

        bool m_goalEditing = false;
        bool m_focusGoalInput = false;
        std::array<char, kGoalCapacity> m_goalBuf{};
    };
} // namespace gladius::ui
