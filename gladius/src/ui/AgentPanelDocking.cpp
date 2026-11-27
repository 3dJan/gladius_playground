#include "AgentPanelDocking.h"

#include "AgentPanelMetrics.h"

#include <algorithm>
#include <imgui_internal.h>
#include <initializer_list>

namespace gladius::ui
{
    namespace
    {
        /// @brief Keeps the panel node free of a tab bar; the panel draws its own header
        void suppressTabBar(ImGuiID nodeID)
        {
            if (ImGuiDockNode * node = ImGui::DockBuilderGetNode(nodeID))
            {
                node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_NoTabBar);
            }
        }

        /**
         * @brief Node that covers the whole dockspace, i.e. the one a window has to fill it.
         * @details After the child split is removed only the root node is left; the central node of
         *          a split dockspace would only cover the leftover middle.
         */
        [[nodiscard]] ImGuiID mainSpaceNodeID(ImGuiID dockspaceID)
        {
            return dockspaceID;
        }
    } // namespace

    void applyAgentPanelDefaultLayout(ImGuiID dockspaceID)
    {
        ImGuiDockNode * root = ImGui::DockBuilderGetNode(dockspaceID);
        if (root == nullptr || root->Size.x <= 0.f || root->Size.y <= 0.f)
        {
            return;
        }

        // The panel keeps a readable width, so the split ratio is derived from a clamped pixel width.
        float const panelWidth =
          std::clamp(root->Size.x * agentMetrics::panelDefaultWidthRatio,
                     agentMetrics::panelMinWidthPx,
                     agentMetrics::panelMaxWidthPx);
        float const splitRatio = panelWidth / root->Size.x;

        ImGuiID rightNodeID = 0;
        ImGuiID centralNodeID = 0;
        ImGui::DockBuilderSplitNode(root->ID, ImGuiDir_Right, splitRatio, &rightNodeID, &centralNodeID);

        // Rule 3 of the dockspace size allocation only honours an explicit SizeRef when the opposite
        // side holds the central node, so pin the width instead of relying on the split ratio alone.
        ImGui::DockBuilderSetNodeSize(rightNodeID, ImVec2(panelWidth, root->Size.y));

        suppressTabBar(rightNodeID);

        ImGui::DockBuilderDockWindow(kAgentPanelWindowName, rightNodeID);
        ImGui::DockBuilderFinish(root->ID);
    }

    void applyCompactLayout(ImGuiID dockspaceID)
    {
        ImGuiDockNode * root = ImGui::DockBuilderGetNode(dockspaceID);
        if (root == nullptr || root->Size.x <= 0.f || root->Size.y <= 0.f)
        {
            return;
        }

        // A phone has no spare column to split off: the panel owns the main space and the camera
        // orbit behind it stays visible through the panel's transparent background. The central node
        // is the node that covers the whole dockspace, so it is the one that actually fills it.
        ImGui::DockBuilderRemoveNodeChildNodes(dockspaceID);
        ImGuiID targetNodeID = dockspaceID;
        if (ImGuiDockNode * centralNode = ImGui::DockBuilderGetCentralNode(dockspaceID))
        {
            targetNodeID = centralNode->ID;
        }
        suppressTabBar(targetNodeID);
        ImGui::DockBuilderDockWindow(kAgentPanelWindowName, targetNodeID);
        ImGui::DockBuilderFinish(root->ID);
    }

    void clearStoredLayout(ImGuiID dockspaceID, std::initializer_list<char const *> windowNames)
    {
        for (char const * name : windowNames)
        {
            if (name != nullptr)
            {
                ImGui::ClearWindowSettings(name);
            }
        }

        // The saved dock tree is applied on top of whatever the DockBuilder produced, so the stored
        // split has to go as well. Flattening keeps the root node alive so the caller can still
        // rebuild the layout in the same frame.
        ImGui::DockBuilderRemoveNodeChildNodes(dockspaceID);
    }

    void applyLayoutForProfile(ImGuiID dockspaceID, SurfaceProfile profile)
    {
        if (profile == SurfaceProfile::Compact)
        {
            applyCompactLayout(dockspaceID);
            return;
        }
        applyAgentPanelDefaultLayout(dockspaceID);
    }

    void showWindowInMainSpace(ImGuiID dockspaceID,
                               char const * windowName,
                               std::initializer_list<char const *> otherMainSpaceWindows)
    {
        if (windowName == nullptr)
        {
            return;
        }

        // A stored DockId is what makes a returning window float, so every window competing for the
        // single main-space slot has to drop its dock reference before the new one takes it.
        auto const undock = [](char const * name)
        {
            if (name == nullptr)
            {
                return;
            }
            if (ImGuiWindow * window = ImGui::FindWindowByName(name))
            {
                // Clears the runtime dock state and the stored reference in one go.
                ImGui::DockContextProcessUndockWindow(ImGui::GetCurrentContext(), window, true);
            }
            ImGui::ClearWindowSettings(name);
        };

        undock(windowName);
        for (char const * other : otherMainSpaceWindows)
        {
            undock(other);
        }

        ImGui::DockBuilderRemoveNodeChildNodes(dockspaceID);
        ImGui::DockBuilderDockWindow(windowName, mainSpaceNodeID(dockspaceID));
        ImGui::DockBuilderFinish(dockspaceID);
    }
} // namespace gladius::ui
