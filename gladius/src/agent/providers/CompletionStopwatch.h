#pragma once

#include "IAiProvider.h"

#include <chrono>
#include <cstddef>

namespace gladius::agent
{
    /**
     * @brief Fills a `CompletionTiming` from wall-clock marks taken inside a provider.
     * @details Each provider owns the moment its HTTP request starts, so the stopwatch is restarted
     *          right before the request; everything else is derived from event arrival.
     */
    class CompletionStopwatch
    {
      public:
        using Clock = std::chrono::steady_clock;

        void restart() noexcept
        {
            m_start = Clock::now();
        }

        [[nodiscard]] std::int64_t elapsedMs() const noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_start)
              .count();
        }

        /// @brief Call once per streamed event; the first one is the time to first token
        void onEvent(CompletionTiming & timing) noexcept
        {
            ++timing.streamEvents;
            if (timing.streamEvents == 1U)
            {
                timing.ttftMs = elapsedMs();
            }
        }

        /// @brief Call on the first event that carries generated output, reasoning included
        void onToken(CompletionTiming & timing) noexcept
        {
            if (timing.firstTokenMs == 0)
            {
                timing.firstTokenMs = elapsedMs();
            }
        }

        /// @brief Call on the first event that carries visible output (text, thinking or tool args)
        void onContent(CompletionTiming & timing) noexcept
        {
            onToken(timing);
            if (timing.firstContentMs == 0)
            {
                timing.firstContentMs = elapsedMs();
            }
        }

        /// @brief Stamp the end of the completion
        void onFinish(CompletionTiming & timing) const noexcept
        {
            timing.totalMs = elapsedMs();
        }

      private:
        Clock::time_point m_start = Clock::now();
    };
} // namespace gladius::agent
