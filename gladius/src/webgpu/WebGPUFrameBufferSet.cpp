#include "webgpu/WebGPUFrameBufferSet.h"
#include "webgpu/WebGPUBufferCapacity.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gladius::webgpu
{
    void WebGPUFrameBufferSet::resize(wgpu::Device const & device,
                                       std::uint32_t const width,
                                       std::uint32_t const height,
                                       std::size_t const parameterCount)
    {
        auto const dispatchSize = calculateSliceDispatchSize(width, height);
        if (!dispatchSize.has_value())
        {
            throw std::invalid_argument("Invalid WebGPU frame dimensions");
        }

        auto const parameterSizeBytes = std::max(sizeof(float), parameterCount * sizeof(float));
        if (m_width == width && m_height == height && m_parameterSizeBytes == parameterSizeBytes && isValid())
        {
            return;
        }

        if (!m_uniformBuffer)
        {
            wgpu::BufferDescriptor uniformDescriptor;
            uniformDescriptor.label = "Gladius frame uniforms";
            uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            uniformDescriptor.size = sizeof(FrameUniforms);
            auto uniformBuffer = device.CreateBuffer(&uniformDescriptor);
            if (!uniformBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame uniforms");
            }
            m_uniformBuffer = std::move(uniformBuffer);
        }

        if (!m_outputBuffer || !m_stagingBuffer ||
            m_outputCapacitySizeBytes < dispatchSize->outputSizeBytes)
        {
            wgpu::BufferDescriptor outputDescriptor;
            outputDescriptor.label = "Gladius frame output";
            outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
            auto const outputCapacitySizeBytes =
              detail::growBufferCapacity(m_outputCapacitySizeBytes, dispatchSize->outputSizeBytes);
            outputDescriptor.size = outputCapacitySizeBytes;
            auto outputBuffer = device.CreateBuffer(&outputDescriptor);

            wgpu::BufferDescriptor stagingDescriptor;
            stagingDescriptor.label = "Gladius frame readback";
            stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            stagingDescriptor.size = outputCapacitySizeBytes;
            auto stagingBuffer = device.CreateBuffer(&stagingDescriptor);
            if (!outputBuffer || !stagingBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame output buffers");
            }

            m_outputBuffer = std::move(outputBuffer);
            m_stagingBuffer = std::move(stagingBuffer);
            m_outputCapacitySizeBytes = outputCapacitySizeBytes;
        }

        if (!m_parameterBuffer || m_parameterCapacitySizeBytes < parameterSizeBytes)
        {
            wgpu::BufferDescriptor parameterDescriptor;
            parameterDescriptor.label = "Gladius frame parameters";
            parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            auto const parameterCapacitySizeBytes =
              detail::growBufferCapacity(m_parameterCapacitySizeBytes, parameterSizeBytes);
            parameterDescriptor.size = parameterCapacitySizeBytes;
            auto parameterBuffer = device.CreateBuffer(&parameterDescriptor);
            if (!parameterBuffer)
            {
                throw std::runtime_error("Unable to allocate WebGPU frame parameters");
            }
            m_parameterBuffer = std::move(parameterBuffer);
            m_parameterCapacitySizeBytes = parameterCapacitySizeBytes;
        }

        if (!isValid())
        {
            throw std::runtime_error("Unable to allocate WebGPU frame buffers");
        }

        m_width = width;
        m_height = height;
        m_outputSizeBytes = dispatchSize->outputSizeBytes;
        m_parameterSizeBytes = parameterSizeBytes;
    }

    void WebGPUFrameBufferSet::writeUniforms(wgpu::Queue const & queue, FrameUniforms const & uniforms) const
    {
        queue.WriteBuffer(m_uniformBuffer, 0u, &uniforms, sizeof(uniforms));
    }

    void WebGPUFrameBufferSet::writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters) const
    {
        float const zero{};
        auto const * data = parameters.empty() ? &zero : parameters.data();
        auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
        queue.WriteBuffer(m_parameterBuffer, 0u, data, size);
    }

    void WebGPUFrameBufferSet::setMeshPayloads(wgpu::Device const & device,
                                               wgpu::Queue const & queue,
                                               std::vector<std::vector<float>> const & meshPayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(meshPayloadTable.size() * 2u, 0u);
        std::size_t totalFloats = 0u;
        for (std::size_t slot = 0u; slot < meshPayloadTable.size(); ++slot)
        {
            auto const & payload = meshPayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
            {
                throw std::length_error("WebGPU mesh payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalFloats += payload.size();
        }

        if (totalFloats == 0u)
        {
            m_meshPayloadBuffer = nullptr;
            m_meshOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalFloats * sizeof(float);
        if (!m_meshPayloadBuffer || m_meshPayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius mesh payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_meshPayloadBuffer = device.CreateBuffer(&descriptor);
            m_meshPayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_meshOffsetTableBuffer || m_meshOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius mesh offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_meshOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_meshOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t offset = 0u;
        for (auto const & payload : meshPayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(float);
            queue.WriteBuffer(m_meshPayloadBuffer, offset, payload.data(), bytes);
            offset += bytes;
        }
    }

    void WebGPUFrameBufferSet::setBeamPayloads(wgpu::Device const & device,
                                               wgpu::Queue const & queue,
                                               std::vector<std::vector<float>> const & beamPayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(beamPayloadTable.size() * 2u, 0u);
        std::size_t totalFloats = 0u;
        for (std::size_t slot = 0u; slot < beamPayloadTable.size(); ++slot)
        {
            auto const & payload = beamPayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
            {
                throw std::length_error("WebGPU beam payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalFloats += payload.size();
        }

        if (totalFloats == 0u)
        {
            m_beamPayloadBuffer = nullptr;
            m_beamOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalFloats * sizeof(float);
        if (!m_beamPayloadBuffer || m_beamPayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius beam payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_beamPayloadBuffer = device.CreateBuffer(&descriptor);
            m_beamPayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_beamOffsetTableBuffer || m_beamOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius beam offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_beamOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_beamOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t beamOffset = 0u;
        for (auto const & payload : beamPayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(float);
            queue.WriteBuffer(m_beamPayloadBuffer, beamOffset, payload.data(), bytes);
            beamOffset += bytes;
        }
    }

    void WebGPUFrameBufferSet::setImagePayloads(
      wgpu::Device const & device,
      wgpu::Queue const & queue,
      std::vector<std::vector<float>> const & imagePayloadTable)
    {
        std::vector<std::uint32_t> offsetTable(imagePayloadTable.size() * 2u, 0u);
        std::size_t totalFloats = 0u;
        for (std::size_t slot = 0u; slot < imagePayloadTable.size(); ++slot)
        {
            auto const & payload = imagePayloadTable[slot];
            if (payload.empty())
            {
                continue;
            }
            if (totalFloats > std::numeric_limits<std::uint32_t>::max() ||
                payload.size() > std::numeric_limits<std::uint32_t>::max() - totalFloats)
            {
                throw std::length_error("WebGPU image payload table exceeds 32-bit addressing");
            }
            offsetTable[slot * 2u] = static_cast<std::uint32_t>(totalFloats);
            offsetTable[slot * 2u + 1u] = static_cast<std::uint32_t>(payload.size());
            totalFloats += payload.size();
        }

        if (totalFloats == 0u)
        {
            m_imagePayloadBuffer = nullptr;
            m_imageOffsetTableBuffer = nullptr;
            return;
        }

        auto const totalBytes = totalFloats * sizeof(float);
        if (!m_imagePayloadBuffer || m_imagePayloadBufferSize < totalBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius image payloads";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = totalBytes;
            m_imagePayloadBuffer = device.CreateBuffer(&descriptor);
            m_imagePayloadBufferSize = totalBytes;
        }

        auto const offsetTableBytes = offsetTable.size() * sizeof(std::uint32_t);
        if (!m_imageOffsetTableBuffer || m_imageOffsetTableBuffer.GetSize() < offsetTableBytes)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius image offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTableBytes;
            m_imageOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_imageOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTableBytes);

        std::size_t imageOffset = 0u;
        for (auto const & payload : imagePayloadTable)
        {
            if (payload.empty())
            {
                continue;
            }
            auto const bytes = payload.size() * sizeof(float);
            queue.WriteBuffer(m_imagePayloadBuffer, imageOffset, payload.data(), bytes);
            imageOffset += bytes;
        }
    }

    void WebGPUFrameBufferSet::reset()
    {
        m_stagingBuffer = nullptr;
        m_parameterBuffer = nullptr;
        m_outputBuffer = nullptr;
        m_uniformBuffer = nullptr;
        m_meshPayloadBuffer = nullptr;
        m_meshOffsetTableBuffer = nullptr;
        m_meshPayloadBufferSize = 0u;
        m_beamPayloadBuffer = nullptr;
        m_beamOffsetTableBuffer = nullptr;
        m_beamPayloadBufferSize = 0u;
        m_imagePayloadBuffer = nullptr;
        m_imageOffsetTableBuffer = nullptr;
        m_imagePayloadBufferSize = 0u;
        m_width = 0u;
        m_height = 0u;
        m_outputSizeBytes = 0u;
        m_outputCapacitySizeBytes = 0u;
        m_parameterSizeBytes = 0u;
        m_parameterCapacitySizeBytes = 0u;
    }

    bool WebGPUFrameBufferSet::isValid() const noexcept
    {
        return m_uniformBuffer && m_outputBuffer && m_stagingBuffer && m_parameterBuffer;
    }

    std::size_t WebGPUFrameBufferSet::getOutputSizeBytes() const noexcept { return m_outputSizeBytes; }
    std::size_t WebGPUFrameBufferSet::getOutputCapacitySizeBytes() const noexcept
    {
        return m_outputCapacitySizeBytes;
    }
    std::size_t WebGPUFrameBufferSet::getParameterSizeBytes() const noexcept { return m_parameterSizeBytes; }
    std::size_t WebGPUFrameBufferSet::getParameterCapacitySizeBytes() const noexcept
    {
        return m_parameterCapacitySizeBytes;
    }
    wgpu::Buffer const & WebGPUFrameBufferSet::getUniformBuffer() const noexcept { return m_uniformBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getOutputBuffer() const noexcept { return m_outputBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getStagingBuffer() const noexcept { return m_stagingBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getParameterBuffer() const noexcept { return m_parameterBuffer; }
}