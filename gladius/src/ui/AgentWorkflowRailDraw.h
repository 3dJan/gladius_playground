#pragma once

#include "AgentWorkflow.h"

#include <imgui.h>

namespace gladius::ui
{
    /// @brief One phase node of the progress rail
    struct RailCircle
    {
        ImVec2 center{};
        float radius = 0.f;
    };

    /// @brief Accent colour of the active theme, shared by the rail and its artifacts
    [[nodiscard]] ImVec4 railAccentColor();

    /// @brief Colour of a finished phase
    [[nodiscard]] ImVec4 railCompletedColor();

    /// @brief Colour of a phase that needs the user's attention
    [[nodiscard]] ImVec4 railAttentionColor();

    /// @brief Node radius for a phase; layout and drawing must agree on it
    [[nodiscard]] float railCircleRadius(PhaseState state);

    /**
     * @brief Draws one leg of the progress line between two node centres.
     * @details Solid accent for legs the workflow already travelled, dashed border colour for the
     *          legs still ahead. Both are drawn at the rail's x so the line stays one straight spine.
     */
    void drawRailLeg(ImDrawList & drawList, float railX, float fromY, float toY, bool travelled);

    /**
     * @brief Draws one phase node over the progress line.
     * @param pulse 0..1 completion pulse of the previous transition
     * @param breath 0..1 slow breathing cycle of the active node
     */
    void drawRailCircle(ImDrawList & drawList,
                        RailCircle const & circle,
                        PhaseState state,
                        float pulse,
                        float breath);
} // namespace gladius::ui
