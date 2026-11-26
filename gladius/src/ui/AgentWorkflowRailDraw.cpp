#include "AgentWorkflowRailDraw.h"

#include "AgentPanelMetrics.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace gladius::ui
{
    namespace
    {
        constexpr ImVec4 kCompletedColor = {0.36f, 0.66f, 0.42f, 1.00f};
        constexpr ImVec4 kAttentionColor = {0.86f, 0.62f, 0.22f, 1.00f};

        ImVec4 scaled(ImVec4 color, float alpha)
        {
            color.w *= alpha;
            return color;
        }

        ImU32 toU32(ImVec4 color, float alpha)
        {
            return ImGui::GetColorU32(scaled(color, alpha));
        }
    } // namespace

    ImVec4 railAccentColor()
    {
        return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
    }

    ImVec4 railCompletedColor()
    {
        return kCompletedColor;
    }

    ImVec4 railAttentionColor()
    {
        return kAttentionColor;
    }

    float railCircleRadius(PhaseState state)
    {
        float const radius = agentMetrics::railRadius * ImGui::GetFontSize();
        return state == PhaseState::Active ? radius * 1.12f : radius;
    }

    void drawRailLeg(ImDrawList & drawList, float railX, float fromY, float toY, bool travelled)
    {
        float const fontSize = ImGui::GetFontSize();
        float const thickness = agentMetrics::railLineWidth * fontSize;
        if (toY <= fromY)
        {
            return;
        }

        // One continuous spine from node centre to node centre. The nodes paint an opaque disc on
        // top, so the stroke reads as running straight through the whole rail.
        drawList.AddLine(ImVec2(railX, fromY),
                         ImVec2(railX, toY),
                         toU32(ImGui::GetStyleColorVec4(ImGuiCol_Border), 0.55f),
                         thickness);

        if (travelled)
        {
            drawList.AddLine(ImVec2(railX, fromY),
                             ImVec2(railX, toY),
                             toU32(railAccentColor(), 0.62f),
                             thickness);
            return;
        }

        // Dashes over the faint spine mark the part of the workflow that is still ahead.
        float const dashLength = agentMetrics::railDashOn * fontSize;
        float const period = dashLength + agentMetrics::railDashOff * fontSize;
        ImU32 const color = ImGui::GetColorU32(ImGuiCol_Border);
        for (float y = fromY; y < toY; y += period)
        {
            float const dashEnd = std::min(toY, y + dashLength);
            drawList.AddLine(ImVec2(railX, y), ImVec2(railX, dashEnd), color, thickness);
        }
    }

    void drawRailCircle(ImDrawList & drawList,
                        RailCircle const & circle,
                        PhaseState state,
                        float pulse,
                        float breath)
    {
        using namespace agentMetrics;
        ImVec2 const c = circle.center;
        float const r = circle.radius;
        ImVec4 const accent = railAccentColor();
        float const fontSize = ImGui::GetFontSize();
        float const unit = fontSize * 0.06f;
        ImU32 const backdrop = ImGui::GetColorU32(ImGuiCol_WindowBg);

        // Opaque disc first: the continuous rail line has to disappear inside the node.
        drawList.AddCircleFilled(c, r, backdrop, 40);

        if (pulse > 0.001f)
        {
            drawList.AddCircle(c,
                               r + (2.f + pulse * 5.f) * unit,
                               toU32(accent, 0.40f * pulse),
                               36,
                               1.2f * unit);
        }

        float const ringThickness = 1.6f * unit;
        switch (state)
        {
        case PhaseState::Pending:
            drawList.AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Border), 36, ringThickness);
            break;
        case PhaseState::Completed:
            drawList.AddCircle(c, r, ImGui::GetColorU32(kCompletedColor), 36, 2.0f * unit);
            {
                float const s = r * 0.62f;
                ImU32 const ink = ImGui::GetColorU32(kCompletedColor);
                float const inkThickness = 1.8f * unit;
                drawList.AddLine(ImVec2(c.x - s, c.y + s * 0.02f),
                                 ImVec2(c.x - s * 0.22f, c.y + s * 0.66f), ink, inkThickness);
                drawList.AddLine(ImVec2(c.x - s * 0.22f, c.y + s * 0.66f),
                                 ImVec2(c.x + s * 0.90f, c.y - s * 0.66f), ink, inkThickness);
            }
            break;
        case PhaseState::Attention:
            drawList.AddCircle(c, r, ImGui::GetColorU32(kAttentionColor), 36, 2.0f * unit);
            {
                ImU32 const ink = ImGui::GetColorU32(kAttentionColor);
                drawList.AddLine(ImVec2(c.x, c.y - r * 0.48f),
                                 ImVec2(c.x, c.y + r * 0.14f), ink, 1.8f * unit);
                drawList.AddCircleFilled(ImVec2(c.x, c.y + r * 0.46f), 1.5f * unit, ink);
            }
            break;
        case PhaseState::Active:
            // Soft outer glow, accent ring and a bright inner ring: the one live node.
            float const glowRadius = r + glowRingRadius * fontSize;
            drawList.AddCircleFilled(c, glowRadius, toU32(accent, 0.14f + 0.10f * breath), 48);
            drawList.AddCircleFilled(c, r, backdrop, 40);
            drawList.AddCircle(c, r, toU32(accent, 0.95f), 40, 2.2f * unit);
            drawList.AddCircle(c, r * 0.62f, ImGui::GetColorU32(ImGuiCol_Text), 32, 1.7f * unit);
            break;
        }
    }
} // namespace gladius::ui
