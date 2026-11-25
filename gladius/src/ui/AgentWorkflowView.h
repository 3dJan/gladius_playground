#pragma once

#include "AgentWorkflow.h"

#include <array>
#include <cstddef>
#include <functional>
#include <imgui.h>
#include <string>

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

      private:
        struct RowLayout
        {
            ImVec2 center{};
            float radius = 0.0f;
            float rowTop = 0.0f;
            float rowBottom = 0.0f;
            float contentBottom = 0.0f;
            bool expanded = false;
            bool hovered = false;
            bool clicked = false;
        };

        void renderCircle(ImDrawList & drawList,
                          RowLayout const & row,
                          PhaseState state,
                          float pulse,
                          float breath) const;

        void renderConnector(ImDrawList & drawList,
                             std::array<RowLayout, kWorkflowPhaseCount> const & rows,
                             std::size_t activeIndex,
                             AgentWorkflowAnimationState & animation,
                             float nowSeconds) const;
    };
} // namespace gladius::ui
