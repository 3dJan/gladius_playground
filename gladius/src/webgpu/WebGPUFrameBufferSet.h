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

        /// Upload mesh payloads (concatenated, one entry per resource slot).
        /// @param meshPayloadTable Flattened payload data; empty entries are zero-length.
        void setMeshPayloads(wgpu::Device const & device,
                             wgpu::Queue const & queue,
                             std::vector<std::vector<float>> const & meshPayloadTable);

        /// Upload beam lattice payloads (concatenated, one entry per resource slot).
        void setBeamPayloads(wgpu::Device const & device,
                             wgpu::Queue const & queue,
                             std::vector<std::vector<float>> const & beamPayloadTable);

        /// Upload image stack payloads (concatenated, one entry per resource slot).
        void setImagePayloads(wgpu::Device const & device,
                              wgpu::Queue const & queue,
                              std::vector<std::vector<float>> const & imagePayloadTable);

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::size_t getOutputSizeBytes() const noexcept;
        [[nodiscard]] std::size_t getParameterSizeBytes() const noexcept;
        [[nodiscard]] bool hasMeshPayloads() const noexcept { return m_meshPayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getUniformBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getOutputBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getStagingBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getParameterBuffer() const noexcept;
        [[nodiscard]] wgpu::Buffer const & getMeshPayloadBuffer() const noexcept
        {
            return m_meshPayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getMeshOffsetTableBuffer() const noexcept
        {
            return m_meshOffsetTableBuffer;
        }
        [[nodiscard]] bool hasBeamPayloads() const noexcept { return m_beamPayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getBeamPayloadBuffer() const noexcept
        {
            return m_beamPayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getBeamOffsetTableBuffer() const noexcept
        {
            return m_beamOffsetTableBuffer;
        }
        [[nodiscard]] bool hasImagePayloads() const noexcept { return m_imagePayloadBuffer != nullptr; }
        [[nodiscard]] wgpu::Buffer const & getImagePayloadBuffer() const noexcept
        {
            return m_imagePayloadBuffer;
        }
        [[nodiscard]] wgpu::Buffer const & getImageOffsetTableBuffer() const noexcept
        {
            return m_imageOffsetTableBuffer;
        }

      private:
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::size_t m_outputSizeBytes{};
        std::size_t m_parameterSizeBytes{};
        wgpu::Buffer m_uniformBuffer;
        wgpu::Buffer m_outputBuffer;
        wgpu::Buffer m_stagingBuffer;
        wgpu::Buffer m_parameterBuffer;
        wgpu::Buffer m_meshPayloadBuffer;
        wgpu::Buffer m_meshOffsetTableBuffer;
        std::size_t m_meshPayloadBufferSize{};
        wgpu::Buffer m_beamPayloadBuffer;
        wgpu::Buffer m_beamOffsetTableBuffer;
        std::size_t m_beamPayloadBufferSize{};
        wgpu::Buffer m_imagePayloadBuffer;
        wgpu::Buffer m_imageOffsetTableBuffer;
        std::size_t m_imagePayloadBufferSize{};
    };
}