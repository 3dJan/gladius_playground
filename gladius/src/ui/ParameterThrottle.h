#pragma once

#include <chrono>

namespace gladius::ui
{
    /// Debounce controller for the parameter-change → recompile pipeline.
    /// Accumulates parameter changes and coalesces recompile triggers
    /// to prevent excessive GPU recompilation during rapid dragging.
    class ParameterThrottle
    {
      public:
        explicit ParameterThrottle(
          std::chrono::milliseconds debounceInterval = std::chrono::milliseconds(100));

        /// Called when a parameter value changes.
        /// @return true if a recompile should be triggered immediately (first call).
        bool onParameterChanged();

        /// Called each frame to check if the debounce window has expired.
        /// @return true if a pending recompile should fire now.
        bool shouldRecompile();

        /// Returns whether a deferred recompile is currently queued.
        [[nodiscard]] bool hasPendingRecompile() const;

        /// Update the coalescing window used for subsequent changes.
        void setDebounceInterval(std::chrono::milliseconds debounceInterval);

        /// Return the current coalescing window.
        [[nodiscard]] std::chrono::milliseconds debounceInterval() const;

        /// Reset the throttle state, clearing any pending recompile.
        void reset();

      private:
        std::chrono::steady_clock::time_point m_lastChangeTime{};
        std::chrono::milliseconds m_debounceInterval;
        bool m_pendingRecompile = false;
        bool m_firstCall = true;
    };
} // namespace gladius::ui
