#pragma once

#include "compute/IBoundsService.h"

#include <memory>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::compute
{
    /**
     * @brief Backend-neutral adapter around the established OpenCL bounds implementation.
     *
     * The adapter deliberately preserves ComputeCore's numerical algorithm as the OpenCL oracle,
     * but exposes its provenance through BoundsResult. In particular, the historical build volume
     * is retained only as an explicitly non-authoritative outcome.
     */
    class OpenCLBoundsService final : public IBoundsService
    {
      public:
        explicit OpenCLBoundsService(std::shared_ptr<gladius::ComputeCore> core);

        [[nodiscard]] RendererCapability getCapabilities() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        void setSceneSnapshot(std::shared_ptr<const RenderSceneSnapshot> snapshot) noexcept override;
        [[nodiscard]] std::optional<BoundsResult>
        getCachedResult(RenderFreshnessStamp const & freshness) const noexcept override;
        [[nodiscard]] std::unique_ptr<IBoundsSubmission> submit(BoundsRequest request) override;

      private:
        struct State;

        std::shared_ptr<gladius::ComputeCore> m_core;
        std::shared_ptr<State> m_state;
    };
}
