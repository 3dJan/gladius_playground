#pragma once

#include "GpuAccessCoordinator.h"
#include "gpgpu.h"

#include <string>
#include <vector>

namespace gladius
{
    class ComputeContext;

    struct GpuKernelResourceAccess
    {
        GpuResourceHandle resource{};
        GpuAccessMode mode{GpuAccessMode::Read};
    };

    class GpuKernelAccessGuard
    {
      public:
        GpuKernelAccessGuard(ComputeContext & context,
                             cl::CommandQueue const & queue,
                             std::string operationName,
                             std::vector<GpuKernelResourceAccess> accesses);
        ~GpuKernelAccessGuard();

        GpuKernelAccessGuard(GpuKernelAccessGuard const &) = delete;
        GpuKernelAccessGuard & operator=(GpuKernelAccessGuard const &) = delete;
        GpuKernelAccessGuard(GpuKernelAccessGuard &&) = delete;
        GpuKernelAccessGuard & operator=(GpuKernelAccessGuard &&) = delete;

        [[nodiscard]] bool granted() const noexcept;
        [[nodiscard]] GpuAccessStatus status() const noexcept;
        [[nodiscard]] std::vector<cl::Event> const & waitEvents() const noexcept;
        [[nodiscard]] size_t waitEventCount() const noexcept;

        void complete(cl::Event const & event);
        void abandon();

      private:
        [[nodiscard]] static std::vector<GpuKernelResourceAccess>
        mergeAccesses(std::vector<GpuKernelResourceAccess> accesses);
        [[nodiscard]] static GpuAccessMode mergeMode(GpuAccessMode lhs, GpuAccessMode rhs) noexcept;

        ComputeContext & m_context;
        std::string m_operationName;
        GpuAccessStatus m_status{GpuAccessStatus::Granted};
        std::vector<GpuAccessToken> m_tokens;
        std::vector<GpuEventId> m_waitEventIds;
        std::vector<cl::Event> m_waitEvents;
        bool m_completed{false};
    };
}
