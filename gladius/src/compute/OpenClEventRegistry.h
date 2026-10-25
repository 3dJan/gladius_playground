#pragma once

#include "GpuAccessCoordinator.h"
#include "gpgpu.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace gladius
{
    class OpenClEventRegistry
    {
      public:
        [[nodiscard]] GpuEventId record(cl::Event const & event);
        [[nodiscard]] std::vector<cl::Event> eventsFor(std::vector<GpuEventId> const & eventIds) const;
        [[nodiscard]] std::vector<GpuEventId> pollCompleted();
        [[nodiscard]] std::vector<GpuEventId> waitFor(std::vector<GpuEventId> const & eventIds);
        [[nodiscard]] std::vector<GpuEventId> waitForAll();
        [[nodiscard]] bool contains(GpuEventId eventId) const;

      private:
        [[nodiscard]] static bool isComplete(cl::Event const & event) noexcept;

        mutable std::mutex m_mutex;
        GpuEventId m_nextEventId{1u};
        std::unordered_map<GpuEventId, cl::Event> m_events;
    };
}
