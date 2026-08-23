#pragma once

#include "compute/IComputeRenderer.h"
#include "webgpu/WebGPUComputeBackend.h"

#include <memory>

namespace gladius::webgpu
{
    /**
     * @brief Materializes analytic WGSL scene snapshots and submits them through Dawn/WebGPU.
     *
    * This renderer accepts analytic full-frame and progressive row-range requests. Low-resolution
    * requests use the same evaluator at the coordinator-provided viewport dimensions.
     */
    class WebGPUComputeRenderer final : public compute::IComputeRenderer
    {
      public:
        explicit WebGPUComputeRenderer(std::shared_ptr<WebGPUComputeContext> context = {});

        [[nodiscard]] compute::ComputeBackendKind getBackendKind() const noexcept override;
        [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        [[nodiscard]] std::unique_ptr<compute::IRenderScene>
        materializeScene(compute::RenderSceneSnapshot snapshot) override;
        [[nodiscard]] std::unique_ptr<compute::IRenderScene> materializeScene(
          std::shared_ptr<const compute::RenderSceneSnapshot> snapshot) override;
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitFrame(compute::IRenderScene const & scene, compute::RenderRequest request) override;
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitFrame(compute::RenderRequest request) override;

        [[nodiscard]] std::shared_ptr<WebGPUComputeContext> getContext() const noexcept
        {
          return m_backend ? m_backend->getContext() : nullptr;
        }

      private:
        std::shared_ptr<WebGPUComputeBackend> m_backend;
    };
}
