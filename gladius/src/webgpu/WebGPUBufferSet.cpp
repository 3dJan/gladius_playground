#include "webgpu/WebGPUBufferSet.h"

#include <stdexcept>

namespace gladius::webgpu
{
    void WebGPUBufferSet::resize(wgpu::Device const & device,
                                  std::uint32_t const width,
                                  std::uint32_t const height,
                                  std::size_t const parameterCount)
    {
        auto const dispatchSize = calculateSliceDispatchSize(width, height);
        if (!dispatchSize.has_value())
        {
            throw std::invalid_argument("Invalid WebGPU slice dimensions");
        }

        auto const parameterSizeBytes = std::max(sizeof(float), parameterCount * sizeof(float));
        if (m_width == width && m_height == height && m_parameterSizeBytes == parameterSizeBytes && isValid())
        {
            return;
        }

        reset();

        wgpu::BufferDescriptor uniformDescriptor;
        uniformDescriptor.label = "Gladius slice uniforms";
        uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniformDescriptor.size = sizeof(SliceUniforms);
        m_uniformBuffer = device.CreateBuffer(&uniformDescriptor);

        wgpu::BufferDescriptor outputDescriptor;
        outputDescriptor.label = "Gladius slice output";
        outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        outputDescriptor.size = dispatchSize->outputSizeBytes;
        m_outputBuffer = device.CreateBuffer(&outputDescriptor);

        wgpu::BufferDescriptor stagingDescriptor;
        stagingDescriptor.label = "Gladius slice readback";
        stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        stagingDescriptor.size = dispatchSize->outputSizeBytes;
        m_stagingBuffer = device.CreateBuffer(&stagingDescriptor);

        wgpu::BufferDescriptor parameterDescriptor;
        parameterDescriptor.label = "Gladius slice parameters";
        parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        parameterDescriptor.size = parameterSizeBytes;
        m_parameterBuffer = device.CreateBuffer(&parameterDescriptor);

        if (!isValid())
        {
            reset();
            throw std::runtime_error("Unable to allocate WebGPU slice buffers");
        }

        m_width = width;
        m_height = height;
        m_outputSizeBytes = dispatchSize->outputSizeBytes;
        m_parameterSizeBytes = parameterSizeBytes;
    }

    void WebGPUBufferSet::writeUniforms(wgpu::Queue const & queue, SliceUniforms const & uniforms) const
    {
        if (!m_uniformBuffer)
        {
            throw std::logic_error("WebGPU slice uniform buffer is not allocated");
        }

        queue.WriteBuffer(m_uniformBuffer, 0u, &uniforms, sizeof(uniforms));
    }

    void WebGPUBufferSet::writeParameters(wgpu::Queue const & queue, std::vector<float> const & parameters) const
    {
        if (!m_parameterBuffer)
        {
            throw std::logic_error("WebGPU slice parameter buffer is not allocated");
        }

        float const zero{};
        auto const * data = parameters.empty() ? &zero : parameters.data();
        auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
        queue.WriteBuffer(m_parameterBuffer, 0u, data, size);
    }

    void WebGPUBufferSet::reset()
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

    bool WebGPUBufferSet::isValid() const noexcept
    {
        return m_uniformBuffer && m_outputBuffer && m_stagingBuffer && m_parameterBuffer;
    }

    std::uint32_t WebGPUBufferSet::getWidth() const noexcept
    {
        return m_width;
    }

    std::uint32_t WebGPUBufferSet::getHeight() const noexcept
    {
        return m_height;
    }

    std::size_t WebGPUBufferSet::getOutputSizeBytes() const noexcept
    {
        return m_outputSizeBytes;
    }

    std::size_t WebGPUBufferSet::getParameterSizeBytes() const noexcept
    {
        return m_parameterSizeBytes;
    }

    wgpu::Buffer const & WebGPUBufferSet::getUniformBuffer() const noexcept
    {
        return m_uniformBuffer;
    }

    wgpu::Buffer const & WebGPUBufferSet::getOutputBuffer() const noexcept
    {
        return m_outputBuffer;
    }

    wgpu::Buffer const & WebGPUBufferSet::getStagingBuffer() const noexcept
    {
        return m_stagingBuffer;
    }

    wgpu::Buffer const & WebGPUBufferSet::getParameterBuffer() const noexcept
    {
        return m_parameterBuffer;
    }
}
