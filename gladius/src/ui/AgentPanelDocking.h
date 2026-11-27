#pragma once

#include <imgui.h>

namespace gladius::ui
{
    /// @brief Name of the Design Agent panel window as submitted to ImGui::Begin()
    inline constexpr char const * kAgentPanelWindowName = "Design Agent";

    /**
     * @brief Build the default Design Agent layout: a right-hand dock column in the main dockspace.
     * @param dockspaceID ID of the main window dockspace node.
     * @details Applied once, only while the user has no stored settings for the panel, so a layout the
     *          user changed by hand (including a floated panel) is preserved on later runs. The docked
     *          node is flagged @c ImGuiDockNodeFlags_NoTabBar because the panel draws its own header.
     */
    void applyAgentPanelDefaultLayout(ImGuiID dockspaceID);
} // namespace gladius::ui
