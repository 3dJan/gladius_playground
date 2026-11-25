#pragma once

#include "AgentWorkflow.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gladius::ui
{
    /**
     * @brief Pure presentation timing for the workflow rail.
     * @details Never mutates agent state: it only remembers when the UI last saw a phase or an
     *          artifact change, and interpolates visuals from the wall clock. When the agent moves
     *          faster than the animation, the newest transition simply replaces the old one instead
     *          of queueing, so the UI catches up rather than lagging behind execution.
     */
    class AgentWorkflowAnimationState
    {
      public:
        /// @brief Duration of the brighten/pulse when a phase finishes
        static constexpr float kCompletionPulseSeconds = 0.20f;
        /// @brief Duration of the light travelling down the connector to the next phase
        static constexpr float kLineTransitionSeconds = 0.42f;
        /// @brief Duration of the breathing ring on the active phase
        static constexpr float kActiveCycleSeconds = 2.2f;
        /// @brief Duration of the fade/slide-in for newly appearing artifacts
        static constexpr float kArtifactArrivalSeconds = 0.20f;

        /**
         * @brief Advances the animation for a newly rendered frame.
         * @param activeIndex Displayed index of the currently active phase
         * @param states Derived state of every phase, used to spot completions
         * @param nowMonotonicSeconds Wall clock in seconds (ImGui::GetTime)
         */
        void update(std::size_t activeIndex,
                    std::array<PhaseState, kWorkflowPhaseCount> const & states,
                    float nowMonotonicSeconds);

        /// @brief Marks an artifact as newly appeared so it can fade in; ignores known keys
        void noteArtifact(std::string const & key, float nowMonotonicSeconds);

        /// @brief 0..1 arrival progress of an artifact; 1 for anything seen long ago
        [[nodiscard]] float artifactArrival(std::string const & key,
                                            float nowMonotonicSeconds) const;

        /// @brief True while the connector into the active phase should carry a light pulse
        [[nodiscard]] bool lineTransitionActive() const noexcept
        {
            return m_lineStartIndex < kWorkflowPhaseCount;
        }
        [[nodiscard]] std::size_t lineTransitionFrom() const noexcept
        {
            return m_lineStartIndex;
        }
        /// @brief 0..1 position of the travelling pulse
        [[nodiscard]] float lineTransitionProgress(float nowMonotonicSeconds) const;

        /// @brief 0..1 brighten amount of a phase's completion pulse
        [[nodiscard]] float completionPulse(std::size_t index, float nowMonotonicSeconds) const;

        /// @brief 0..1 phase of the active circle's breathing ring
        [[nodiscard]] float activeBreath(float nowMonotonicSeconds) const;

        /// @brief Forget all timings, e.g. when a fresh design run starts
        void reset() noexcept;

      private:
        std::size_t m_lastActiveIndex = 0;
        std::size_t m_lineStartIndex = kWorkflowPhaseCount;
        float m_lineStartTime = 0.0f;
        std::array<float, kWorkflowPhaseCount> m_completionTimes{};
        std::array<PhaseState, kWorkflowPhaseCount> m_lastStates{};
        bool m_initialised = false;
        std::unordered_map<std::string, float> m_artifactAppearTimes;
    };
} // namespace gladius::ui
