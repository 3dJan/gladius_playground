#include "AgentWorkflowAnimation.h"

#include <algorithm>
#include <cmath>

namespace gladius::ui
{
    namespace
    {
        float clampUnit(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }
    } // namespace

    void AgentWorkflowAnimationState::update(
      std::size_t activeIndex,
      std::array<PhaseState, kWorkflowPhaseCount> const & states,
      float nowMonotonicSeconds)
    {
        if (!m_initialised)
        {
            m_lastActiveIndex = activeIndex;
            m_lastStates = states;
            m_completionTimes.fill(-1.0f);
            m_initialised = true;
            return;
        }

        for (std::size_t i = 0; i < kWorkflowPhaseCount; ++i)
        {
            bool const justCompleted = m_lastStates[i] != PhaseState::Completed
                                       && states[i] == PhaseState::Completed;
            if (justCompleted)
            {
                m_completionTimes[i] = nowMonotonicSeconds;
            }
            else if (states[i] != PhaseState::Completed)
            {
                m_completionTimes[i] = -1.0f;
            }
        }

        if (activeIndex != m_lastActiveIndex && m_lastActiveIndex < activeIndex)
        {
            // A later phase became active: restart the pulse so a burst of state changes never
            // leaves several queued transitions behind.
            m_lineStartIndex = activeIndex > 0 ? activeIndex - 1 : 0;
            m_lineStartTime = nowMonotonicSeconds;
        }
        else if (activeIndex != m_lastActiveIndex)
        {
            m_lineStartIndex = kWorkflowPhaseCount;
        }

        m_lastActiveIndex = activeIndex;
        m_lastStates = states;
    }

    void AgentWorkflowAnimationState::noteArtifact(std::string const & key, float nowMonotonicSeconds)
    {
        if (m_artifactAppearTimes.contains(key))
        {
            return;
        }
        m_artifactAppearTimes.emplace(key, nowMonotonicSeconds);
        // Bound the map for long sessions with many artifacts.
        constexpr std::size_t kKeepMostRecent = 512;
        if (m_artifactAppearTimes.size() > kKeepMostRecent)
        {
            auto oldest = m_artifactAppearTimes.begin();
            for (auto it = m_artifactAppearTimes.begin(); it != m_artifactAppearTimes.end(); ++it)
            {
                if (it->second < oldest->second)
                {
                    oldest = it;
                }
            }
            m_artifactAppearTimes.erase(oldest);
        }
    }

    float AgentWorkflowAnimationState::artifactArrival(std::string const & key,
                                                       float nowMonotonicSeconds) const
    {
        auto const it = m_artifactAppearTimes.find(key);
        if (it == m_artifactAppearTimes.end())
        {
            return 1.0f;
        }
        return clampUnit((nowMonotonicSeconds - it->second) / kArtifactArrivalSeconds);
    }

    float AgentWorkflowAnimationState::lineTransitionProgress(float nowMonotonicSeconds) const
    {
        if (m_lineStartIndex >= kWorkflowPhaseCount)
        {
            return 1.0f;
        }
        float const t = (nowMonotonicSeconds - m_lineStartTime) / kLineTransitionSeconds;
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        return clampUnit(t);
    }

    float AgentWorkflowAnimationState::completionPulse(std::size_t index,
                                                       float nowMonotonicSeconds) const
    {
        if (index >= kWorkflowPhaseCount || m_completionTimes[index] < 0.0f)
        {
            return 0.0f;
        }
        float const t = (nowMonotonicSeconds - m_completionTimes[index]) / kCompletionPulseSeconds;
        if (t < 0.0f || t > 1.0f)
        {
            return 0.0f;
        }
        // Rise and fall, so the circle brightens once and settles.
        return 1.0f - std::abs(1.0f - 2.0f * t);
    }

    float AgentWorkflowAnimationState::activeBreath(float nowMonotonicSeconds) const
    {
        float const phase = std::fmod(nowMonotonicSeconds, kActiveCycleSeconds) / kActiveCycleSeconds;
        return 0.5f + 0.5f * std::sin(phase * 2.0f * 3.14159265358979323846f);
    }

    void AgentWorkflowAnimationState::reset() noexcept
    {
        m_lastActiveIndex = 0;
        m_lineStartIndex = kWorkflowPhaseCount;
        m_lineStartTime = 0.0f;
        m_completionTimes.fill(-1.0f);
        m_lastStates.fill(PhaseState::Pending);
        m_artifactAppearTimes.clear();
        m_initialised = false;
    }
} // namespace gladius::ui
