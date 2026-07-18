#pragma once

#include "compute/IComputeRenderer.h"
#include "compute/RenderSession.h"

namespace gladius::compute
{
    /**
     * @brief Backend-neutral adapter for a retained OpenCL render session.
     *
     * The adapter only advertises paths it can execute without changing the retained scene:
     * analytic full-model and hybrid row-range frames. Precomputed-SDF and distance-initialized
     * rendering require dedicated payloads and are rejected rather than silently downgraded.
     */
    class OpenCLComputeRenderer final : public IComputeRenderer
    {
      public:
        explicit OpenCLComputeRenderer(SharedRenderSession session);

        [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override;
        [[nodiscard]] RendererCapability getCapabilities() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        [[nodiscard]] std::unique_ptr<IRenderScene> materializeScene(RenderSceneSnapshot snapshot) override;
        [[nodiscard]] std::unique_ptr<IRenderSubmission>
        submitFrame(IRenderScene const & scene, RenderRequest request) override;
        [[nodiscard]] std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest request) override;

      private:
        SharedRenderSession m_session;
    };
}