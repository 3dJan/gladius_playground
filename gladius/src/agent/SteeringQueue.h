#pragma once

#include "Conversation.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Steering message a user submits while a run is already in progress
    struct SteeringMessage
    {
        std::string text;
        std::vector<ImageContent> images;
        std::chrono::steady_clock::time_point submittedAt{};
    };

    /// @brief Upper bound on steering messages kept for a single run
    /// @details Every queued message is re-sent on all later turns, so an unbounded queue turns a
    ///          chatty user into unbounded prompt cost.
    inline constexpr std::size_t kMaxSteersPerRun = 8;

    /**
     * @brief Thread-safe mailbox between the UI thread and the run thread.
     * @details The UI thread only ever pushes; the run thread only ever drains, and it does so at a
     *          turn boundary where appending to the conversation is both API-legal and cache-safe.
     *          Everything else the agent owns (design graph, todos, document) stays single-threaded.
     */
    class SteeringQueue
    {
      public:
        explicit SteeringQueue(std::size_t maxQueued = kMaxSteersPerRun) noexcept;

        /// @brief Queues one steering message; false when the run already holds enough
        bool push(SteeringMessage message);

        /// @brief Removes and returns every queued message in submission order (run thread only)
        [[nodiscard]] std::vector<SteeringMessage> drain();

        /// @brief Messages waiting; safe to call from the UI thread for status display
        [[nodiscard]] std::size_t size() const;

        /// @brief True when nothing is waiting
        [[nodiscard]] bool empty() const;

        /// @brief Drops all queued messages, e.g. because the run was stopped
        void abandon();

        /// @brief Drops all queued messages and reopens the queue for a new run
        void reset() noexcept;

        /// @brief Message budget of this queue
        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return m_maxQueued;
        }

      private:
        mutable std::mutex m_mutex;
        std::vector<SteeringMessage> m_messages;
        std::size_t m_maxQueued;
    };
} // namespace gladius::agent
