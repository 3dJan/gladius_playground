#pragma once

#include <atomic>

namespace gladius::io
{
    /// @brief Thread-safe cooperative cancellation signal
    ///
    /// Lightweight wrapper around atomic bool for communicating cancellation
    /// requests from UI thread to worker threads. The token is owned by the
    /// dialog and shared with the exporter via pointer.
    ///
    /// Usage:
    /// - UI thread calls requestCancellation() when user clicks Cancel
    /// - Worker thread calls isCancelled() at checkpoints to detect cancellation
    /// - Dialog calls reset() before starting a new export
    class CancellationToken
    {
      public:
        /// @brief Request cancellation (called from UI thread)
        void requestCancellation()
        {
            m_cancelled.store(true, std::memory_order_release);
        }

        /// @brief Check if cancellation was requested (called from worker thread)
        /// @return true if cancellation was requested
        [[nodiscard]] bool isCancelled() const
        {
            return m_cancelled.load(std::memory_order_acquire);
        }

        /// @brief Reset for reuse (called before new export)
        void reset()
        {
            m_cancelled.store(false, std::memory_order_release);
        }

      private:
        std::atomic<bool> m_cancelled{false};
    };

} // namespace gladius::io
