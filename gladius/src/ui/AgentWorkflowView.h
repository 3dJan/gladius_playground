#pragma once

#include "AgentWorkflow.h"
#include "AgentWorkflowRailDraw.h"

#include <array>
#include <cstddef>
#include <functional>
#include <imgui.h>
#include <string>
#include <utility>

namespace gladius::ui
{
    class AgentWorkflowAnimationState;

    /**
     * @brief Renders the vertical phase rail: circles on one continuous line, each with its own
     *        expandable artifact section.
     * @details Pure presentation. It never touches agent state: the caller supplies the derived
     *          phase states, the expansion flags it owns, and a callback per phase that draws that
     *          phase's artifacts indented to the right of the rail.
     */
    class AgentWorkflowView
    {
      public:
        /// @brief Draws one phase's artifacts, already indented right of the progress line
        using ArtifactRenderer = std::function<void()>;

        /**
         * @brief Renders the workflow tree into the current (scrollable) ImGui window.
         * @param states Derived visual state of every phase
         * @param activeIndex Index of the phase the agent is working on
         * @param summaries Restrained status lines for the active/attention phase; may be empty
         * @param expanded In/out expansion state owned by the host
         * @param renderers Artifact drawing callback per phase
         * @param animation Presentation-only transition state owned by the host
         */
        void render(std::array<PhaseState, kWorkflowPhaseCount> const & states,
                    std::size_t activeIndex,
                    std::array<std::string, kWorkflowPhaseCount> const & summaries,
                    std::array<bool, kWorkflowPhaseCount> & expanded,
                    std::array<ArtifactRenderer, kWorkflowPhaseCount> const & renderers,
                    AgentWorkflowAnimationState & animation);

        /// @brief Horizontal space the rail occupies before artifact content starts
        [[nodiscard]] static float contentIndent() noexcept;

        /// @brief Geometry the last render pass measured for one phase row
        struct RowLayout
        {
            ImVec2 center{};
            float railX = 0.0f;
            float contentX = 0.0f;
            float radius = 0.0f;
            float rowTop = 0.0f;
            float rowBottom = 0.0f;
            float contentBottom = 0.0f;
            bool expanded = false;
            bool hovered = false;
            bool clicked = false;
        };

        /// @brief Rows of the last render pass; lets the host and the tests assert on rail geometry
        [[nodiscard]] std::array<RowLayout, kWorkflowPhaseCount> const & lastLayout() const noexcept
        {
            return m_rows;
        }

        /// @brief Spine segments (fromY, toY) drawn between consecutive nodes by the last render
        [[nodiscard]] std::array<std::pair<float, float>, kWorkflowPhaseCount - 1> const &
          lastSpine() const noexcept
        {
            return m_spine;
        }

      private:
        void renderGlow(ImDrawList & drawList,
                        std::array<RowLayout, kWorkflowPhaseCount> const & rows,
                        AgentWorkflowAnimationState & animation,
                        float nowSeconds) const;

        std::array<RowLayout, kWorkflowPhaseCount> m_rows{};
        std::array<std::pair<float, float>, kWorkflowPhaseCount - 1> m_spine{};
    };
} // namespace gladius::ui
