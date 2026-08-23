#pragma once

#include "compute/IComputeBackend.h"
#include "webgpu/WebGPUComputeContext.h"

#include <memory>

namespace gladius::webgpu
{
    /**
     * @brief Native Dawn implementation of the API-independent slice backend.
     */
    class WebGPUComputeBackend final : public compute::IComputeBackend
    {
      public:
        explicit WebGPUComputeBackend(std::shared_ptr<WebGPUComputeContext> context = {});

        [[nodiscard]] compute::ComputeBackendKind getKind() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        [[nodiscard]] std::shared_ptr<WebGPUComputeContext> getContext() const noexcept
        {
          return m_context;
        }
        [[nodiscard]] std::unique_ptr<compute::ISliceSubmission>
        submitSlice(compute::SliceRequest request) override;
        [[nodiscard]] std::unique_ptr<compute::IFrameSubmission>
        submitFrame(compute::FrameRequest request) override;

      private:
        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
