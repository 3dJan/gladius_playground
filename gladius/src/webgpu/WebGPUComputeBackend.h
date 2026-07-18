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
        WebGPUComputeBackend();

        [[nodiscard]] compute::ComputeBackendKind getKind() const noexcept override;
        [[nodiscard]] bool isAvailable() const noexcept override;
        [[nodiscard]] std::unique_ptr<compute::ISliceSubmission>
        submitSlice(compute::SliceRequest request) override;

      private:
        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
