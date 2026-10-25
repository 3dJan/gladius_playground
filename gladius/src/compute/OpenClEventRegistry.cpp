#include "OpenClEventRegistry.h"

#include <algorithm>

namespace gladius
{
    GpuEventId OpenClEventRegistry::record(cl::Event const & event)
    {
        if (event() == nullptr)
        {
            return 0u;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        GpuEventId const eventId = m_nextEventId++;
        m_events.emplace(eventId, event);
        return eventId;
    }

    std::vector<cl::Event>
    OpenClEventRegistry::eventsFor(std::vector<GpuEventId> const & eventIds) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<cl::Event> events;
        events.reserve(eventIds.size());
        for (auto const eventId : eventIds)
        {
            auto const eventIt = m_events.find(eventId);
            if (eventIt != m_events.end() && eventIt->second() != nullptr)
            {
                events.push_back(eventIt->second);
            }
        }
        return events;
    }

    std::vector<GpuEventId> OpenClEventRegistry::pollCompleted()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<GpuEventId> completed;
        for (auto eventIt = m_events.begin(); eventIt != m_events.end();)
        {
            if (isComplete(eventIt->second))
            {
                completed.push_back(eventIt->first);
                eventIt = m_events.erase(eventIt);
            }
            else
            {
                ++eventIt;
            }
        }
        return completed;
    }

    std::vector<GpuEventId> OpenClEventRegistry::waitFor(std::vector<GpuEventId> const & eventIds)
    {
        auto events = eventsFor(eventIds);
        for (auto const & event : events)
        {
            if (event() != nullptr)
            {
                event.wait();
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<GpuEventId> completed;
        completed.reserve(eventIds.size());
        for (auto const eventId : eventIds)
        {
            auto const eventIt = m_events.find(eventId);
            if (eventIt != m_events.end())
            {
                completed.push_back(eventId);
                m_events.erase(eventIt);
            }
        }
        return completed;
    }

    std::vector<GpuEventId> OpenClEventRegistry::waitForAll()
    {
        std::vector<GpuEventId> eventIds;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            eventIds.reserve(m_events.size());
            for (auto const & [eventId, event] : m_events)
            {
                (void) event;
                eventIds.push_back(eventId);
            }
        }

        return waitFor(eventIds);
    }

    bool OpenClEventRegistry::contains(GpuEventId const eventId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events.find(eventId) != m_events.end();
    }

    bool OpenClEventRegistry::isComplete(cl::Event const & event) noexcept
    {
        if (event() == nullptr)
        {
            return true;
        }

        try
        {
            auto const status = event.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
            return status == CL_COMPLETE;
        }
        catch (...)
        {
            return true;
        }
    }
}
