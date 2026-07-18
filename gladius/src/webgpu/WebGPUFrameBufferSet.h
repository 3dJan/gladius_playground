#pragma once

#include "webgpu/WebGPUDispatchPolicy.h"
#include "webgpu/WebGPUShaderAbi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace gladius::webgpu
{
    /**
     * @brief Owns buffers required by one headless WebGPU frame render.
     */
    class WebGPUFrameBufferSet
    {
      public:
        void resize(wgpu::Device const & device,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::size_t parameterCount);
        void writeUniforms(wgpu::Queue const & queue, FrameUniforms const & uniforms) const;
        void writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters) const;
        void reset();

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::size_t getOutputSizeBytes() const noexcept;
        [[nodiscard]] std::size_t getParameterSizeBytes() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getUniformBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getOutputBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getStagingBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getParameterBuffer() const noexcept;

      private:
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::size_t m_outputSizeBytes{};
        std::size_t m_parameterSizeBytes{};
        wgpu::Buffer m_uniformBuffer;
        wgpu::Buffer m_outputBuffer;
        wgpu::Buffer m_stagingBuffer;
        wgpu::Buffer m_parameterBuffer;
    };
}