#pragma once

#include "../io/CancellationToken.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <utility>

namespace gladius::agent
{
    /**
     * @brief Cooperative pause/resume signal for a running agent loop.
     * @details Pause is only observed at turn and tool-call boundaries, so a pause request never
     *          interrupts a tool that is mid-execution. Cancellation stays independent: a paused
     *          run can always be stopped.
     */
    class AgentRunControl
    {
      public:
        /// @brief Awaitable that parks the loop until the run is resumed or cancelled
        class PauseAwaitable
        {
          public:
            PauseAwaitable(AgentRunControl & owner, io::CancellationToken const & cancellation)
              : m_owner(owner)
              , m_cancellation(cancellation)
            {
            }

            [[nodiscard]] bool await_ready() const noexcept
            {
                return !m_owner.isPaused() || m_cancellation.isCancelled();
            }

            /**
             * @brief Native builds block the run thread, which carries nothing but the agent loop,
             *        so a condition-variable wait is cheap and keeps the coroutine chain intact.
             *        Web builds must never block the browser thread, so the coroutine parks there
             *        and is resumed by the UI thread.
             */
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle)
            {
#ifndef __EMSCRIPTEN__
                std::unique_lock<std::mutex> lock(m_owner.m_mutex);
                m_owner.m_cv.wait(lock, [this]() {
                    return !m_owner.isPaused() || m_cancellation.isCancelled();
                });
                return handle;
#else
                m_owner.m_suspended = handle;
                return std::noop_coroutine();
#endif
            }

            void await_resume() const noexcept
            {
            }

          private:
            AgentRunControl & m_owner;
            io::CancellationToken const & m_cancellation;
        };

        /// @brief Ask the loop to stop at the next turn or tool-call boundary
        void requestPause() noexcept
        {
            m_paused.store(true, std::memory_order_release);
        }

        /**
         * @brief Let a paused run continue.
         * @param unlimitedFromNow When true, the loop lifts its remaining limits instead of only
         *        getting a fresh budget for the next segment.
         */
        void resume(bool unlimitedFromNow = false) noexcept
        {
            m_continueUnlimited.store(unlimitedFromNow, std::memory_order_release);
            m_paused.store(false, std::memory_order_release);
            m_cv.notify_all();
            resumeSuspended();
        }

        [[nodiscard]] bool isPaused() const noexcept
        {
            return m_paused.load(std::memory_order_acquire);
        }

        /// @brief Reads and clears the "continue unlimited" intent set by resume()
        bool consumeContinueUnlimited() noexcept
        {
            return m_continueUnlimited.exchange(false, std::memory_order_acq_rel);
        }

        /// @brief Clears pause state and any parked coroutine, e.g. before a new run
        void reset() noexcept
        {
            m_paused.store(false, std::memory_order_release);
            m_continueUnlimited.store(false, std::memory_order_release);
            m_suspended = {};
        }

        [[nodiscard]] PauseAwaitable
          waitForResume(io::CancellationToken const & cancellation) noexcept
        {
            return PauseAwaitable{*this, cancellation};
        }

      private:
        /// @brief Resumes a parked coroutine on platforms that cannot block
        void resumeSuspended()
        {
            auto handle = std::exchange(m_suspended, {});
            if (handle)
            {
                handle.resume();
            }
        }

        std::atomic<bool> m_paused{false};
        std::atomic<bool> m_continueUnlimited{false};
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::coroutine_handle<> m_suspended;
    };
} // namespace gladius::agent
