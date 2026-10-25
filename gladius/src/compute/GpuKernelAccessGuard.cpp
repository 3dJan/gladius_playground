#include "GpuKernelAccessGuard.h"

#include "ComputeContext.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace gladius
{
    namespace
    {
        void appendUnique(std::vector<GpuEventId> & target, std::vector<GpuEventId> const & source)
        {
            for (auto const eventId : source)
            {
                if (eventId == 0u)
                {
                    continue;
                }

                if (std::find(target.begin(), target.end(), eventId) == target.end())
                {
                    target.push_back(eventId);
                }
            }
        }
    }

    GpuKernelAccessGuard::GpuKernelAccessGuard(ComputeContext & context,
                                               cl::CommandQueue const & queue,
                                               std::string operationName,
                                               std::vector<GpuKernelResourceAccess> accesses)
        : m_context(context)
        , m_operationName(std::move(operationName))
    {
        if (m_context.isGpuAccessSafeModeEnabled())
        {
            m_context.waitForAllTrackedGpuWork();
        }

        m_context.refreshGpuAccessEvents();

        auto const queueId = m_context.getGpuQueueId(queue);
        auto mergedAccesses = mergeAccesses(std::move(accesses));
        m_tokens.reserve(mergedAccesses.size());

        for (auto const & access : mergedAccesses)
        {
            auto plan = m_context.gpuAccessCoordinator().beginAccess(
              GpuAccessRequest{.resource = access.resource,
                               .queueId = queueId,
                               .mode = access.mode,
                               .operationName = m_operationName});
            if (!plan.granted())
            {
                m_status = plan.status;
                abandon();
                return;
            }

            m_tokens.push_back(plan.token);
            appendUnique(m_waitEventIds, plan.waitEvents);
        }

        m_waitEvents = m_context.gpuWaitEvents(m_waitEventIds);

        if (m_context.isDebugOutputEnabled())
        {
            if (auto logger = m_context.getLogger())
            {
                logger->logInfo(fmt::format("GPU access: op='{}', queue={}, resources={}, waits={}, safeMode={}",
                                            m_operationName,
                                            queueId,
                                            mergedAccesses.size(),
                                            m_waitEvents.size(),
                                            m_context.isGpuAccessSafeModeEnabled()));
            }
        }
    }

    GpuKernelAccessGuard::~GpuKernelAccessGuard()
    {
        if (!m_completed)
        {
            abandon();
        }
    }

    bool GpuKernelAccessGuard::granted() const noexcept
    {
        return m_status == GpuAccessStatus::Granted;
    }

    GpuAccessStatus GpuKernelAccessGuard::status() const noexcept
    {
        return m_status;
    }

    std::vector<cl::Event> const & GpuKernelAccessGuard::waitEvents() const noexcept
    {
        return m_waitEvents;
    }

    size_t GpuKernelAccessGuard::waitEventCount() const noexcept
    {
        return m_waitEvents.size();
    }

    void GpuKernelAccessGuard::complete(cl::Event const & event)
    {
        if (m_completed)
        {
            return;
        }

        auto const eventId = m_context.recordGpuEvent(event);
        for (auto const token : m_tokens)
        {
            (void) m_context.gpuAccessCoordinator().completeAccess(token, eventId);
        }
        if (m_context.isGpuAccessSafeModeEnabled() && eventId != 0u)
        {
            m_context.waitForGpuEvents({eventId});
        }
        m_completed = true;
    }

    void GpuKernelAccessGuard::abandon()
    {
        for (auto const token : m_tokens)
        {
            (void) m_context.gpuAccessCoordinator().completeAccess(token, 0u);
        }
        m_tokens.clear();
        m_completed = true;
    }

    std::vector<GpuKernelResourceAccess>
    GpuKernelAccessGuard::mergeAccesses(std::vector<GpuKernelResourceAccess> accesses)
    {
        std::vector<GpuKernelResourceAccess> merged;
        merged.reserve(accesses.size());

        for (auto const & access : accesses)
        {
            auto existingIt = std::find_if(merged.begin(),
                                           merged.end(),
                                           [&access](GpuKernelResourceAccess const & existing)
                                           { return existing.resource == access.resource; });
            if (existingIt == merged.end())
            {
                merged.push_back(access);
            }
            else
            {
                existingIt->mode = mergeMode(existingIt->mode, access.mode);
            }
        }

        return merged;
    }

    GpuAccessMode GpuKernelAccessGuard::mergeMode(GpuAccessMode const lhs,
                                                  GpuAccessMode const rhs) noexcept
    {
        if (lhs == GpuAccessMode::ReadWrite || rhs == GpuAccessMode::ReadWrite)
        {
            return GpuAccessMode::ReadWrite;
        }

        if (lhs == rhs)
        {
            return lhs;
        }

        return GpuAccessMode::ReadWrite;
    }
}
