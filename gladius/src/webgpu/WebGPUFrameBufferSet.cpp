#include "webgpu/WebGPUFrameBufferSet.h"

#include <algorithm>
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

    void WebGPUFrameBufferSet::reset()
    {
        m_stagingBuffer = nullptr;
        m_parameterBuffer = nullptr;
        m_outputBuffer = nullptr;
        m_uniformBuffer = nullptr;
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