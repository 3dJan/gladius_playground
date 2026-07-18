#pragma once

#include "compute/IComputeRenderer.h"
#include "webgpu/WebGPUComputeBackend.h"

#include <memory>

namespace gladius::webgpu
{
    /**
     * @brief Materializes analytic WGSL scene snapshots and submits them through Dawn/WebGPU.
     *
     * This initial renderer accepts full-frame analytic requests. Progressive row ranges require
     * a dedicated output-row offset in the WGSL frame ABI and remain intentionally unavailable.
     */
    class WebGPUComputeRenderer final : public compute::IComputeRenderer
    {
      public:
        WebGPUComputeRenderer();

        [[nodiscard]] compute::ComputeBackendKind getBackendKind() const noexcept override;
        [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        [[nodiscard]] std::unique_ptr<compute::IRenderScene>
        materializeScene(compute::RenderSceneSnapshot snapshot) override;
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitFrame(compute::IRenderScene const & scene, compute::RenderRequest request) override;
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitFrame(compute::RenderRequest request) override;

      private:
        std::shared_ptr<WebGPUComputeBackend> m_backend;
    };
}
