// Execution Limits section of the Agent Provider Settings panel. Every limit is unlimited-capable
// (0 means unlimited) and written to the config immediately, so a crash or exit never loses the
// choice. The rows live here instead of AgentPanels.cpp to keep that file about run control.

#include "AgentPanels.h"
#include "../ConfigManager.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace gladius::ui
{
    namespace
    {
        constexpr int kFallbackWallTimeMinutes = 15;
        constexpr int kFallbackMaxModelTurns = 64;
        constexpr int kFallbackMaxToolCalls = 256;
        constexpr int kFallbackMaxViewportReviews = 16;

        std::chrono::milliseconds wallTimeFromMinutes(int minutes)
        {
            return std::chrono::milliseconds(std::max(0, minutes) * 60000);
        }

        int minutesFromWallTime(std::chrono::milliseconds const duration)
        {
            return static_cast<int>(duration.count() / 60000);
        }
    } // namespace

    void AgentPanels::renderExecutionLimits()
    {
        auto persist = [this](char const * key, int value) {
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>("Agent", key, value);
                m_configManager->save();
            }
        };

        bool unlimitedTurns = m_limits.maxModelTurns <= 0;
        if (ImGui::Checkbox("Unlimited model turns", &unlimitedTurns))
        {
            m_limits.maxModelTurns = unlimitedTurns ? 0 : kFallbackMaxModelTurns;
            persist("maxModelTurns", m_limits.maxModelTurns);
        }
        if (!unlimitedTurns)
        {
            int maxTurns = m_limits.maxModelTurns;
            if (ImGui::SliderInt("Max model turns", &maxTurns, 1, 1024))
            {
                m_limits.maxModelTurns = maxTurns;
                persist("maxModelTurns", maxTurns);
            }
        }

        bool unlimitedToolCalls = m_limits.maxToolCalls == 0;
        if (ImGui::Checkbox("Unlimited tool calls", &unlimitedToolCalls))
        {
            m_limits.maxToolCalls =
              unlimitedToolCalls ? 0 : static_cast<std::size_t>(kFallbackMaxToolCalls);
            persist("maxToolCalls", static_cast<int>(m_limits.maxToolCalls));
        }
        if (!unlimitedToolCalls)
        {
            int maxToolCalls = static_cast<int>(m_limits.maxToolCalls);
            if (ImGui::SliderInt("Max tool calls", &maxToolCalls, 1, 4096))
            {
                m_limits.maxToolCalls = static_cast<std::size_t>(maxToolCalls);
                persist("maxToolCalls", maxToolCalls);
            }
        }

        bool unlimitedViewportReviews = m_limits.maxViewportReviews == 0;
        if (ImGui::Checkbox("Unlimited viewport reviews", &unlimitedViewportReviews))
        {
            m_limits.maxViewportReviews = 0;
            persist("maxViewportReviews", 0);
        }
        if (!unlimitedViewportReviews)
        {
            int maxViewportReviews = static_cast<int>(m_limits.maxViewportReviews);
            if (ImGui::SliderInt("Max viewport reviews", &maxViewportReviews, 1, 64))
            {
                m_limits.maxViewportReviews = static_cast<std::size_t>(maxViewportReviews);
                persist("maxViewportReviews", maxViewportReviews);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Viewport images the agent may request per segment. Each image costs "
                              "a fixed share of every later turn.");
        }

        bool unlimitedWallTime = m_limits.maxWallTime.count() <= 0;
        if (ImGui::Checkbox("Unlimited wall time", &unlimitedWallTime))
        {
            m_limits.maxWallTime =
              unlimitedWallTime ? std::chrono::milliseconds(0)
                                : wallTimeFromMinutes(kFallbackWallTimeMinutes);
            persist("maxWallTimeMinutes", minutesFromWallTime(m_limits.maxWallTime));
        }
        if (!unlimitedWallTime)
        {
            int wallTimeMinutes = minutesFromWallTime(m_limits.maxWallTime);
            if (ImGui::SliderInt("Wall time limit", &wallTimeMinutes, 1, 720, "%d min"))
            {
                m_limits.maxWallTime = wallTimeFromMinutes(wallTimeMinutes);
                persist("maxWallTimeMinutes", wallTimeMinutes);
            }
        }

        if (ImGui::Button("Make all unlimited"))
        {
            m_limits.maxModelTurns = 0;
            m_limits.maxToolCalls = 0;
            m_limits.maxViewportReviews = 0;
            m_limits.maxWallTime = std::chrono::milliseconds(0);
            persist("maxModelTurns", 0);
            persist("maxToolCalls", 0);
            persist("maxViewportReviews", 0);
            persist("maxWallTimeMinutes", 0);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Long design sessions with local models usually need no limits at "
                              "all; a hit limit parks the run and offers a continue choice.");
        }
    }
} // namespace gladius::ui
