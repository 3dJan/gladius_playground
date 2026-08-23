#pragma once

#include "compute/IBoundsService.h"
#include "webgpu/WebGPUComputeContext.h"

#include <memory>

namespace gladius::webgpu
{
    /**
     * @brief WebGPU analytic bounds service backed by the runtime-owned Dawn context.
     *
     * Bounds are determined by tiled signed-distance probes over the immutable analytic scene
     * snapshot. The service deliberately does not provide a build-volume fallback: an empty or
     * clipped probe is reported as such and must be handled by the caller.
     */
    class WebGPUBoundsService final : public compute::IBoundsService
    {
      public:
        explicit WebGPUBoundsService(std::shared_ptr<WebGPUComputeContext> context);

        [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        void setSceneSnapshot(
          std::shared_ptr<const compute::RenderSceneSnapshot> snapshot) noexcept override;
        [[nodiscard]] std::optional<compute::BoundsResult>
        getCachedResult(compute::RenderFreshnessStamp const & freshness) const noexcept override;
        [[nodiscard]] std::unique_ptr<compute::IBoundsSubmission>
        submit(compute::BoundsRequest request) override;

        [[nodiscard]] std::shared_ptr<WebGPUComputeContext> getContext() const noexcept
        {
          return m_context;
        }

      private:
        struct State;

        std::shared_ptr<WebGPUComputeContext> m_context;
        std::shared_ptr<State> m_state;
    };
}
