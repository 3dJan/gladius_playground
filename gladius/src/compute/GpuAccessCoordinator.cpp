#include "GpuAccessCoordinator.h"

#include <algorithm>
#include <utility>

namespace gladius
{
    namespace
    {
        [[nodiscard]] bool isValidEvent(GpuEventId const eventId) noexcept
        {
            return eventId != 0u;
        }

        void appendUniqueEvent(std::vector<GpuEventId> & events, GpuEventId const eventId)
        {
            if (!isValidEvent(eventId))
            {
                return;
            }

            if (std::find(events.begin(), events.end(), eventId) == events.end())
            {
                events.push_back(eventId);
            }
        }
    }

    GpuResourceHandle GpuAccessCoordinator::registerResource(GpuResourceKind const kind,
                                                             std::string debugName,
                                                             GpuResourceGeneration initialGeneration)
    {
        if (initialGeneration == 0u)
        {
            initialGeneration = 1u;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        GpuResourceId const resourceId = m_nextResourceId++;
        ResourceState resource{};
        resource.resourceId = resourceId;
        resource.currentGeneration = initialGeneration;
        resource.kind = kind;
        resource.debugName = std::move(debugName);
        resource.generations.emplace(initialGeneration,
                                     GenerationState{.generation = initialGeneration});

        m_resources.emplace(resourceId, std::move(resource));
        return GpuResourceHandle{.resourceId = resourceId, .generation = initialGeneration};
    }

    std::optional<GpuResourceHandle>
    GpuAccessCoordinator::currentHandle(GpuResourceId const resourceId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto const resourceIt = m_resources.find(resourceId);
        if (resourceIt == m_resources.end())
        {
            return std::nullopt;
        }

        return GpuResourceHandle{.resourceId = resourceId,
                                 .generation = resourceIt->second.currentGeneration};
    }

    std::optional<GpuResourceDebugInfo>
    GpuAccessCoordinator::debugInfo(GpuResourceId const resourceId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto const resourceIt = m_resources.find(resourceId);
        if (resourceIt == m_resources.end())
        {
            return std::nullopt;
        }

        auto const & resource = resourceIt->second;
        return GpuResourceDebugInfo{.resourceId = resource.resourceId,
                                    .currentGeneration = resource.currentGeneration,
                                    .kind = resource.kind,
                                    .debugName = resource.debugName};
    }

    GpuAccessPlan GpuAccessCoordinator::beginAccess(GpuAccessRequest request)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto resourceIt = m_resources.find(request.resource.resourceId);
        if (resourceIt == m_resources.end())
        {
            return GpuAccessPlan{.status = GpuAccessStatus::UnknownResource};
        }

        auto & resource = resourceIt->second;
        if (request.resource.generation != resource.currentGeneration)
        {
            return GpuAccessPlan{.status = GpuAccessStatus::StaleGeneration};
        }

        auto generationIt = resource.generations.find(request.resource.generation);
        if (generationIt == resource.generations.end() || generationIt->second.retired)
        {
            return GpuAccessPlan{.status = GpuAccessStatus::StaleGeneration};
        }

        auto & generation = generationIt->second;
        pruneCompletedEventsLocked(generation);

        if (hasConflictingPendingAccessLocked(request.resource, request.mode))
        {
            return GpuAccessPlan{.status = GpuAccessStatus::PendingAccessWithoutEvent};
        }

        GpuAccessToken const token = m_nextAccessToken++;
        auto waitEvents = collectDependenciesLocked(generation, request.mode);
        m_activeAccesses.emplace(token,
                                 ActiveAccess{.token = token,
                                              .resource = request.resource,
                                              .queueId = request.queueId,
                                              .mode = request.mode,
                                              .operationName = std::move(request.operationName)});

        return GpuAccessPlan{.status = GpuAccessStatus::Granted,
                             .token = token,
                             .waitEvents = std::move(waitEvents)};
    }

    GpuAccessStatus GpuAccessCoordinator::completeAccess(GpuAccessToken const token,
                                                         GpuEventId const eventId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto activeIt = m_activeAccesses.find(token);
        if (activeIt == m_activeAccesses.end())
        {
            return GpuAccessStatus::UnknownAccessToken;
        }

        auto const active = std::move(activeIt->second);
        m_activeAccesses.erase(activeIt);

        auto resourceIt = m_resources.find(active.resource.resourceId);
        if (resourceIt == m_resources.end())
        {
            return GpuAccessStatus::UnknownResource;
        }

        auto generationIt = resourceIt->second.generations.find(active.resource.generation);
        if (generationIt == resourceIt->second.generations.end())
        {
            return GpuAccessStatus::StaleGeneration;
        }

        if (!isValidEvent(eventId))
        {
            return GpuAccessStatus::Granted;
        }

        auto & generation = generationIt->second;
        if (writesResource(active.mode))
        {
            generation.lastWriterEvent = eventId;
            generation.readerEvents.clear();
        }
        else
        {
            appendUniqueEvent(generation.readerEvents, eventId);
        }

        return GpuAccessStatus::Granted;
    }

    void GpuAccessCoordinator::markEventCompleted(GpuEventId const eventId)
    {
        if (!isValidEvent(eventId))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_completedEvents.insert(eventId);
    }

    void GpuAccessCoordinator::markEventsCompleted(std::vector<GpuEventId> const & eventIds)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto const eventId : eventIds)
        {
            if (isValidEvent(eventId))
            {
                m_completedEvents.insert(eventId);
            }
        }
    }

    bool GpuAccessCoordinator::isEventCompleted(GpuEventId const eventId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return isCompletedLocked(eventId);
    }

    bool GpuAccessCoordinator::isIdle(GpuResourceHandle const resource) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto const resourceIt = m_resources.find(resource.resourceId);
        if (resourceIt == m_resources.end())
        {
            return true;
        }

        auto const generationIt = resourceIt->second.generations.find(resource.generation);
        if (generationIt == resourceIt->second.generations.end())
        {
            return true;
        }

        if (std::any_of(m_activeAccesses.begin(),
                        m_activeAccesses.end(),
                        [resource](auto const & entry) { return entry.second.resource == resource; }))
        {
            return false;
        }

        return outstandingEventsLocked(resource).empty();
    }

    std::vector<GpuEventId>
    GpuAccessCoordinator::outstandingEvents(GpuResourceHandle const resource) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        return outstandingEventsLocked(resource);
    }

    std::vector<GpuEventId>
    GpuAccessCoordinator::outstandingEventsLocked(GpuResourceHandle const resource) const
    {

        auto const resourceIt = m_resources.find(resource.resourceId);
        if (resourceIt == m_resources.end())
        {
            return {};
        }

        auto const generationIt = resourceIt->second.generations.find(resource.generation);
        if (generationIt == resourceIt->second.generations.end())
        {
            return {};
        }

        std::vector<GpuEventId> events;
        auto const & generation = generationIt->second;
        if (generation.lastWriterEvent.has_value() && !isCompletedLocked(*generation.lastWriterEvent))
        {
            appendUniqueEvent(events, *generation.lastWriterEvent);
        }

        for (auto const eventId : generation.readerEvents)
        {
            if (!isCompletedLocked(eventId))
            {
                appendUniqueEvent(events, eventId);
            }
        }

        for (auto const eventId : generation.retirementWaitEvents)
        {
            if (!isCompletedLocked(eventId))
            {
                appendUniqueEvent(events, eventId);
            }
        }

        return events;
    }

    GpuGenerationRetirement
    GpuAccessCoordinator::retireCurrentGeneration(GpuResourceId const resourceId)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto resourceIt = m_resources.find(resourceId);
        if (resourceIt == m_resources.end())
        {
            return GpuGenerationRetirement{.status = GpuAccessStatus::UnknownResource};
        }

        auto & resource = resourceIt->second;
        GpuResourceHandle const retiredHandle{.resourceId = resourceId,
                                              .generation = resource.currentGeneration};

        if (hasConflictingPendingAccessLocked(retiredHandle, GpuAccessMode::Write))
        {
            return GpuGenerationRetirement{.status = GpuAccessStatus::PendingAccessWithoutEvent,
                                           .retiredGeneration = retiredHandle};
        }

        auto generationIt = resource.generations.find(resource.currentGeneration);
        if (generationIt == resource.generations.end())
        {
            return GpuGenerationRetirement{.status = GpuAccessStatus::StaleGeneration,
                                           .retiredGeneration = retiredHandle};
        }

        auto & retiredGeneration = generationIt->second;
        pruneCompletedEventsLocked(retiredGeneration);
        auto waitEvents = collectDependenciesLocked(retiredGeneration, GpuAccessMode::Write);
        retiredGeneration.retired = true;
        retiredGeneration.retirementWaitEvents = waitEvents;

        GpuResourceGeneration const newGeneration = resource.currentGeneration + 1u;
        resource.currentGeneration = newGeneration;
        resource.generations.emplace(newGeneration, GenerationState{.generation = newGeneration});

        return GpuGenerationRetirement{
          .status = GpuAccessStatus::Granted,
          .retiredGeneration = retiredHandle,
          .newGeneration = GpuResourceHandle{.resourceId = resourceId, .generation = newGeneration},
          .waitEvents = std::move(waitEvents)};
    }

    void GpuAccessCoordinator::collectCompletedRetirements()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto & [resourceId, resource] : m_resources)
        {
            (void) resourceId;
            for (auto generationIt = resource.generations.begin();
                 generationIt != resource.generations.end();)
            {
                auto & generation = generationIt->second;
                if (!generation.retired)
                {
                    ++generationIt;
                    continue;
                }

                bool const hasOutstandingRetirementEvent =
                  std::any_of(generation.retirementWaitEvents.begin(),
                              generation.retirementWaitEvents.end(),
                              [this](GpuEventId const eventId)
                              { return !isCompletedLocked(eventId); });

                                GpuResourceId const currentResourceId = resource.resourceId;
                                GpuResourceGeneration const currentGeneration = generation.generation;
                bool const hasActiveAccess =
                  std::any_of(m_activeAccesses.begin(),
                              m_activeAccesses.end(),
                                                            [currentResourceId, currentGeneration](auto const & entry)
                              {
                                                                    return entry.second.resource.resourceId == currentResourceId &&
                                                                                 entry.second.resource.generation == currentGeneration;
                              });

                if (!hasOutstandingRetirementEvent && !hasActiveAccess)
                {
                    generationIt = resource.generations.erase(generationIt);
                }
                else
                {
                    ++generationIt;
                }
            }
        }
    }

    size_t GpuAccessCoordinator::retiredGenerationCount(GpuResourceId const resourceId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto const resourceIt = m_resources.find(resourceId);
        if (resourceIt == m_resources.end())
        {
            return 0u;
        }

        return static_cast<size_t>(std::count_if(
          resourceIt->second.generations.begin(),
          resourceIt->second.generations.end(),
          [](auto const & entry) { return entry.second.retired; }));
    }

    bool GpuAccessCoordinator::isCompletedLocked(GpuEventId const eventId) const
    {
        return !isValidEvent(eventId) || m_completedEvents.find(eventId) != m_completedEvents.end();
    }

    void GpuAccessCoordinator::pruneCompletedEventsLocked(GenerationState & generation) const
    {
        if (generation.lastWriterEvent.has_value() && isCompletedLocked(*generation.lastWriterEvent))
        {
            generation.lastWriterEvent.reset();
        }

        std::erase_if(generation.readerEvents,
                      [this](GpuEventId const eventId) { return isCompletedLocked(eventId); });

        std::erase_if(generation.retirementWaitEvents,
                      [this](GpuEventId const eventId) { return isCompletedLocked(eventId); });
    }

    std::vector<GpuEventId>
    GpuAccessCoordinator::collectDependenciesLocked(GenerationState const & generation,
                                                    GpuAccessMode const mode) const
    {
        std::vector<GpuEventId> dependencies;

        if (generation.lastWriterEvent.has_value() && !isCompletedLocked(*generation.lastWriterEvent))
        {
            appendUniqueEvent(dependencies, *generation.lastWriterEvent);
        }

        if (writesResource(mode))
        {
            for (auto const eventId : generation.readerEvents)
            {
                if (!isCompletedLocked(eventId))
                {
                    appendUniqueEvent(dependencies, eventId);
                }
            }
        }

        return dependencies;
    }

    bool GpuAccessCoordinator::hasConflictingPendingAccessLocked(GpuResourceHandle const resource,
                                                                 GpuAccessMode const mode) const
    {
        return std::any_of(m_activeAccesses.begin(),
                           m_activeAccesses.end(),
                           [resource, mode](auto const & entry)
                           {
                               auto const & active = entry.second;
                               return active.resource == resource && accessesConflict(active.mode, mode);
                           });
    }

    bool GpuAccessCoordinator::accessesConflict(GpuAccessMode const lhs,
                                                GpuAccessMode const rhs) noexcept
    {
        return writesResource(lhs) || writesResource(rhs);
    }

    bool GpuAccessCoordinator::writesResource(GpuAccessMode const mode) noexcept
    {
        return mode == GpuAccessMode::Write || mode == GpuAccessMode::ReadWrite;
    }
}
