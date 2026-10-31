#pragma once

#include <chrono>
#include <optional>

namespace gladius::ui::async_rendering
{
    /// @brief One-shot quiet-period settlement, independent of UI and render backends.
    class ParameterInteractionPolicy
    {
      public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /// @brief Renew the quiet period on every parameter edit.
        void notifyParameterChanged(TimePoint now) noexcept
        {
            m_lastChangeTime = now;
        }

        /// @brief Consume settlement once, at least 200 ms after the latest edit.
        [[nodiscard]] bool consumeSettlement(TimePoint now) noexcept
        {
            constexpr auto QUIET_PERIOD = std::chrono::milliseconds(200);
            if (!m_lastChangeTime.has_value() || now - *m_lastChangeTime < QUIET_PERIOD)
            {
                return false;
            }

            reset();
            return true;
        }

        /// @brief Whether UI frames are still needed to reach the quiet-period deadline.
        [[nodiscard]] bool hasPendingSettlement() const noexcept
        {
            return m_lastChangeTime.has_value();
        }

        /// @brief Disarm on explicit completion or a superseding interaction/document.
        void reset() noexcept
        {
            m_lastChangeTime.reset();
        }

      private:
        std::optional<TimePoint> m_lastChangeTime;
    };
}