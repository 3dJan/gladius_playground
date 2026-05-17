#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

namespace gladius
{
    using GpuResourceId = uint64_t;
    using GpuResourceGeneration = uint64_t;
    using GpuQueueId = uint64_t;
    using GpuEventId = uint64_t;
    using GpuAccessToken = uint64_t;

    enum class GpuResourceKind
    {
        Buffer,
        Image2D,
        Image3D,
        ImageGL,
        HostSnapshot
    };

    enum class GpuAccessMode
    {
        Read,
        Write,
        ReadWrite
    };

    enum class GpuAccessStatus
    {
        Granted,
        UnknownResource,
        StaleGeneration,
        PendingAccessWithoutEvent,
        UnknownAccessToken
    };

    struct GpuResourceHandle
    {
        GpuResourceId resourceId{};
        GpuResourceGeneration generation{};

        [[nodiscard]] friend bool operator==(GpuResourceHandle const & lhs,
                                             GpuResourceHandle const & rhs) noexcept
        {
            return lhs.resourceId == rhs.resourceId && lhs.generation == rhs.generation;
        }
    };

    struct GpuAccessRequest
    {
        GpuResourceHandle resource{};
        GpuQueueId queueId{};
        GpuAccessMode mode{GpuAccessMode::Read};
        std::string operationName;
    };

    struct GpuAccessPlan
    {
        GpuAccessStatus status{GpuAccessStatus::UnknownResource};
        GpuAccessToken token{};
        std::vector<GpuEventId> waitEvents;

        [[nodiscard]] bool granted() const noexcept
        {
            return status == GpuAccessStatus::Granted;
        }
    };

    struct GpuGenerationRetirement
    {
        GpuAccessStatus status{GpuAccessStatus::UnknownResource};
        GpuResourceHandle retiredGeneration{};
        GpuResourceHandle newGeneration{};
        std::vector<GpuEventId> waitEvents;

        [[nodiscard]] bool granted() const noexcept
        {
            return status == GpuAccessStatus::Granted;
        }

        [[nodiscard]] bool canReleaseImmediately() const noexcept
        {
            return granted() && waitEvents.empty();
        }
    };

    struct GpuResourceDebugInfo
    {
        GpuResourceId resourceId{};
        GpuResourceGeneration currentGeneration{};
        GpuResourceKind kind{GpuResourceKind::Buffer};
        std::string debugName;
    };

    class GpuAccessCoordinator
    {
      public:
        [[nodiscard]] GpuResourceHandle registerResource(GpuResourceKind kind,
                                                         std::string debugName,
                                                         GpuResourceGeneration initialGeneration = 1u);

        [[nodiscard]] std::optional<GpuResourceHandle>
        currentHandle(GpuResourceId resourceId) const;

        [[nodiscard]] std::optional<GpuResourceDebugInfo>
        debugInfo(GpuResourceId resourceId) const;

        [[nodiscard]] GpuAccessPlan beginAccess(GpuAccessRequest request);

        [[nodiscard]] GpuAccessStatus completeAccess(GpuAccessToken token, GpuEventId eventId);

        void markEventCompleted(GpuEventId eventId);
        void markEventsCompleted(std::vector<GpuEventId> const & eventIds);

        [[nodiscard]] bool isEventCompleted(GpuEventId eventId) const;

        [[nodiscard]] bool isIdle(GpuResourceHandle resource) const;

        [[nodiscard]] std::vector<GpuEventId> outstandingEvents(GpuResourceHandle resource) const;

        [[nodiscard]] GpuGenerationRetirement retireCurrentGeneration(GpuResourceId resourceId);

        void collectCompletedRetirements();

        [[nodiscard]] size_t retiredGenerationCount(GpuResourceId resourceId) const;

      private:
        struct GenerationState
        {
            GpuResourceGeneration generation{};
            std::optional<GpuEventId> lastWriterEvent;
            std::vector<GpuEventId> readerEvents;
            bool retired{false};
            std::vector<GpuEventId> retirementWaitEvents;
        };

        struct ResourceState
        {
            GpuResourceId resourceId{};
            GpuResourceGeneration currentGeneration{};
            GpuResourceKind kind{GpuResourceKind::Buffer};
            std::string debugName;
            std::unordered_map<GpuResourceGeneration, GenerationState> generations;
        };

        struct ActiveAccess
        {
            GpuAccessToken token{};
            GpuResourceHandle resource{};
            GpuQueueId queueId{};
            GpuAccessMode mode{GpuAccessMode::Read};
            std::string operationName;
        };

        [[nodiscard]] bool isCompletedLocked(GpuEventId eventId) const;
        void pruneCompletedEventsLocked(GenerationState & generation) const;
        [[nodiscard]] std::vector<GpuEventId>
        outstandingEventsLocked(GpuResourceHandle resource) const;
        [[nodiscard]] std::vector<GpuEventId>
        collectDependenciesLocked(GenerationState const & generation, GpuAccessMode mode) const;
        [[nodiscard]] bool hasConflictingPendingAccessLocked(GpuResourceHandle resource,
                                                             GpuAccessMode mode) const;
        [[nodiscard]] static bool accessesConflict(GpuAccessMode lhs, GpuAccessMode rhs) noexcept;
        [[nodiscard]] static bool writesResource(GpuAccessMode mode) noexcept;

        mutable std::mutex m_mutex;
        GpuResourceId m_nextResourceId{1u};
        GpuAccessToken m_nextAccessToken{1u};
        std::unordered_map<GpuResourceId, ResourceState> m_resources;
        std::unordered_map<GpuAccessToken, ActiveAccess> m_activeAccesses;
        std::unordered_set<GpuEventId> m_completedEvents;
    };
}
