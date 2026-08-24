#include "webgpu/WebGPUFrameBufferSet.h"

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

        reset();

        wgpu::BufferDescriptor uniformDescriptor;
        uniformDescriptor.label = "Gladius frame uniforms";
        uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniformDescriptor.size = sizeof(FrameUniforms);
        m_uniformBuffer = device.CreateBuffer(&uniformDescriptor);

        wgpu::BufferDescriptor outputDescriptor;
        outputDescriptor.label = "Gladius frame output";
        outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        outputDescriptor.size = dispatchSize->outputSizeBytes;
        m_outputBuffer = device.CreateBuffer(&outputDescriptor);

        wgpu::BufferDescriptor stagingDescriptor;
        stagingDescriptor.label = "Gladius frame readback";
        stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        stagingDescriptor.size = dispatchSize->outputSizeBytes;
        m_stagingBuffer = device.CreateBuffer(&stagingDescriptor);

        wgpu::BufferDescriptor parameterDescriptor;
        parameterDescriptor.label = "Gladius frame parameters";
        parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        parameterDescriptor.size = parameterSizeBytes;
        m_parameterBuffer = device.CreateBuffer(&parameterDescriptor);

        if (!isValid())
        {
            reset();
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

        if (!m_meshOffsetTableBuffer)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius mesh offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTable.size() * sizeof(std::uint32_t);
            m_meshOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_meshOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTable.size() * sizeof(std::uint32_t));

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

        if (!m_beamOffsetTableBuffer)
        {
            wgpu::BufferDescriptor descriptor;
            descriptor.label = "Gladius beam offset table";
            descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            descriptor.size = offsetTable.size() * sizeof(std::uint32_t);
            m_beamOffsetTableBuffer = device.CreateBuffer(&descriptor);
        }
        queue.WriteBuffer(m_beamOffsetTableBuffer,
                          0u,
                          offsetTable.data(),
                          offsetTable.size() * sizeof(std::uint32_t));

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
        m_width = 0u;
        m_height = 0u;
        m_outputSizeBytes = 0u;
        m_parameterSizeBytes = 0u;
    }

    bool WebGPUFrameBufferSet::isValid() const noexcept
    {
        return m_uniformBuffer && m_outputBuffer && m_stagingBuffer && m_parameterBuffer;
    }

    std::size_t WebGPUFrameBufferSet::getOutputSizeBytes() const noexcept { return m_outputSizeBytes; }
    std::size_t WebGPUFrameBufferSet::getParameterSizeBytes() const noexcept { return m_parameterSizeBytes; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getUniformBuffer() const noexcept { return m_uniformBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getOutputBuffer() const noexcept { return m_outputBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getStagingBuffer() const noexcept { return m_stagingBuffer; }
    wgpu::Buffer const & WebGPUFrameBufferSet::getParameterBuffer() const noexcept { return m_parameterBuffer; }
}