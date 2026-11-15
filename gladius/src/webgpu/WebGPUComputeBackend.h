#pragma once

#include "compute/IComputeBackend.h"
#include "compute/IComputeRenderer.h"
#include "compute/RenderContracts.h"
#include "webgpu/WebGPUComputeContext.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gladius::webgpu
{
  class WebGPUFramePipelineCache;
  struct PrecomputedSdfState;

    struct SdfPrebakeRequest
    {
        std::string shaderSource;
        std::vector<float> parameters;
        std::vector<std::vector<float>> meshPayloads;
        std::vector<std::vector<float>> beamPayloads;
        std::vector<std::vector<std::uint32_t>> imagePayloads;
        compute::RenderBounds bounds{};
        std::uint32_t gridSize{128u};
        std::uint64_t sceneGeneration{0u};
        std::uint64_t parameterGeneration{0u};
    };

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
        [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
        submitSdfPrecomputation(SdfPrebakeRequest request);
        void invalidatePrecomputedSdf() noexcept;
        [[nodiscard]] bool hasValidPrecomputedSdf() const noexcept;

      private:
        class FrameBufferPool;
        std::shared_ptr<WebGPUComputeContext> m_context;
        std::shared_ptr<FrameBufferPool> m_frameBufferPool;
        std::shared_ptr<WebGPUFramePipelineCache> m_framePipelineCache;
        std::shared_ptr<PrecomputedSdfState> m_precomputedSdf;
    };
}
