#pragma once

#include "SurfaceProfile.h"

#include <imgui.h>
#include <initializer_list>

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

    /**
     * @brief Build the compact Design Agent layout: the panel takes the whole main dockspace.
     * @param dockspaceID ID of the main window dockspace node.
     * @details The root node is not split, so the panel covers the main space while the camera orbit
     *          stays visible through the panel's transparent background. The docked node keeps
     *          @c ImGuiDockNodeFlags_NoTabBar for the same reason as the desktop layout.
     */
    void applyCompactLayout(ImGuiID dockspaceID);

    /**
     * @brief Drop the stored layout so the next layout pass starts from scratch.
     * @param dockspaceID ID of the main window dockspace node.
     * @param windowNames Windows whose stored settings should be discarded.
     * @details Used when the surface profile changes (rotate, resize, manual override): without this,
     *          the saved dock tree wins over the new default layout. @c ImGui::ClearWindowSettings also
     *          undocks the window, and the pending delete is honoured on the next settings write.
     */
    void clearStoredLayout(ImGuiID dockspaceID, std::initializer_list<char const *> windowNames);

    /**
     * @brief Applies the default layout that belongs to a profile.
     * @param dockspaceID ID of the main window dockspace node.
     * @param profile Surface the UI is drawn on.
     */
    void applyLayoutForProfile(ImGuiID dockspaceID, SurfaceProfile profile);

    /**
     * @brief Puts a single window into the main space of a compact dockspace.
     * @param dockspaceID ID of the main window dockspace node.
     * @param windowName Window that should fill the main space.
     * @param otherMainSpaceWindows Windows that must give the slot up; their stored dock reference
     *                              is dropped so they do not float back when shown again.
     * @details A phone has room for exactly one main-space window.
     */
    void showWindowInMainSpace(ImGuiID dockspaceID,
                               char const * windowName,
                               std::initializer_list<char const *> otherMainSpaceWindows = {});
} // namespace gladius::ui
