#include "SteeringQueue.h"

#include <utility>

namespace gladius::agent
{
    SteeringQueue::SteeringQueue(std::size_t maxQueued) noexcept
      : m_maxQueued(maxQueued)
    {
    }

    bool SteeringQueue::push(SteeringMessage message)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (m_messages.size() >= m_maxQueued)
        {
            return false;
        }
        if (message.submittedAt == std::chrono::steady_clock::time_point{})
        {
            message.submittedAt = std::chrono::steady_clock::now();
        }
        m_messages.push_back(std::move(message));
        return true;
    }

    std::vector<SteeringMessage> SteeringQueue::drain()
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        std::vector<SteeringMessage> drained;
        drained.swap(m_messages);
        return drained;
    }

    std::size_t SteeringQueue::size() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_messages.size();
    }

    bool SteeringQueue::empty() const
    {
        return size() == 0;
    }

    void SteeringQueue::abandon()
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_messages.clear();
    }

    void SteeringQueue::reset() noexcept
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_messages.clear();
    }
} // namespace gladius::agent
