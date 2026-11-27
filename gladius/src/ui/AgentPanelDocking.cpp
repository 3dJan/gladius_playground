#include "AgentPanelDocking.h"

#include "AgentPanelMetrics.h"

#include <algorithm>
#include <imgui_internal.h>

namespace gladius::ui
{
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

        if (ImGuiDockNode * rightNode = ImGui::DockBuilderGetNode(rightNodeID))
        {
            // The panel brings its own header; without a tab bar there is also no corner triangle.
            rightNode->SetLocalFlags(rightNode->LocalFlags | ImGuiDockNodeFlags_NoTabBar);
        }

        ImGui::DockBuilderDockWindow(kAgentPanelWindowName, rightNodeID);
        ImGui::DockBuilderFinish(root->ID);
    }
} // namespace gladius::ui
