#include "webgpu/WebGPUSdfEvaluator.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::uint32_t WORKGROUP_SIZE = 64u;

        struct alignas(16) EvaluationUniforms
        {
            float isoValue{};
            std::uint32_t pointCount{};
            std::uint32_t reserved0{};
            std::uint32_t reserved1{};
        };

        static_assert(sizeof(EvaluationUniforms) == 16u);

        struct alignas(16) EvaluationPosition
        {
            std::array<float, 4> value{};
        };

        class EvaluationBuffers final
        {
          public:
            void create(wgpu::Device const & device,
                        std::size_t const pointCount,
                        std::size_t const parameterCount)
            {
                auto const parameterSize = std::max(sizeof(float), parameterCount * sizeof(float));
                auto const positionSize = pointCount * sizeof(EvaluationPosition);
                auto const outputSize = pointCount * sizeof(float);

                wgpu::BufferDescriptor uniformDescriptor;
                uniformDescriptor.label = "Gladius SDF evaluation uniforms";
                uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
                uniformDescriptor.size = sizeof(EvaluationUniforms);
                m_uniform = device.CreateBuffer(&uniformDescriptor);

                wgpu::BufferDescriptor positionDescriptor;
                positionDescriptor.label = "Gladius SDF evaluation positions";
                positionDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                positionDescriptor.size = positionSize;
                m_positions = device.CreateBuffer(&positionDescriptor);

                wgpu::BufferDescriptor outputDescriptor;
                outputDescriptor.label = "Gladius SDF evaluation output";
                outputDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
                outputDescriptor.size = outputSize;
                m_output = device.CreateBuffer(&outputDescriptor);

                wgpu::BufferDescriptor stagingDescriptor;
                stagingDescriptor.label = "Gladius SDF evaluation readback";
                stagingDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
                stagingDescriptor.size = outputSize;
                m_staging = device.CreateBuffer(&stagingDescriptor);

                wgpu::BufferDescriptor parameterDescriptor;
                parameterDescriptor.label = "Gladius SDF evaluation parameters";
                parameterDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
                parameterDescriptor.size = parameterSize;
                m_parameters = device.CreateBuffer(&parameterDescriptor);

                if (!m_uniform || !m_positions || !m_output || !m_staging || !m_parameters)
                {
                    throw std::runtime_error("Unable to allocate WebGPU SDF evaluation buffers");
                }

                m_outputSize = outputSize;
                m_parameterSize = parameterSize;
            }

            void write(wgpu::Queue const & queue,
                       EvaluationUniforms const & uniforms,
                       std::vector<EvaluationPosition> const & positions,
                       std::vector<float> const & parameters) const
            {
                queue.WriteBuffer(m_uniform, 0u, &uniforms, sizeof(uniforms));
                queue.WriteBuffer(m_positions,
                                  0u,
                                  positions.data(),
                                  positions.size() * sizeof(EvaluationPosition));

                float const zero{};
                auto const * data = parameters.empty() ? &zero : parameters.data();
                auto const size = parameters.empty() ? sizeof(zero) : parameters.size() * sizeof(float);
                queue.WriteBuffer(m_parameters, 0u, data, size);
            }

            [[nodiscard]] std::size_t outputSize() const noexcept { return m_outputSize; }
            [[nodiscard]] std::size_t parameterSize() const noexcept { return m_parameterSize; }
            [[nodiscard]] wgpu::Buffer const & uniform() const noexcept { return m_uniform; }
            [[nodiscard]] wgpu::Buffer const & positions() const noexcept { return m_positions; }
            [[nodiscard]] wgpu::Buffer const & output() const noexcept { return m_output; }
            [[nodiscard]] wgpu::Buffer const & staging() const noexcept { return m_staging; }
            [[nodiscard]] wgpu::Buffer const & parameters() const noexcept { return m_parameters; }

          private:
            std::size_t m_outputSize{};
            std::size_t m_parameterSize{};
            wgpu::Buffer m_uniform;
            wgpu::Buffer m_positions;
            wgpu::Buffer m_output;
            wgpu::Buffer m_staging;
            wgpu::Buffer m_parameters;
        };
    }

    WebGPUSdfEvaluator::WebGPUSdfEvaluator()
        : m_context(std::make_shared<WebGPUComputeContext>())
    {
    }

    WebGPUSdfEvaluator::WebGPUSdfEvaluator(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(std::move(context))
    {
    }

    bool WebGPUSdfEvaluator::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    std::string const & WebGPUSdfEvaluator::getErrorMessage() const noexcept
    {
        static std::string const empty;
        return m_context ? m_context->getErrorMessage() : empty;
    }

    compute::SdfEvaluationResult WebGPUSdfEvaluator::evaluate(
      compute::SdfEvaluationRequest request) const
    {
        if (request.positions.empty())
        {
            return {};
        }
        if (!isAvailable())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        if (request.shaderSource.empty())
        {
            throw std::invalid_argument("WebGPU SDF evaluator shader source must not be empty");
        }
        if (request.positions.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::invalid_argument("WebGPU SDF evaluation point count is too large");
        }

        std::vector<EvaluationPosition> positions;
        positions.reserve(request.positions.size());
        for (auto const & position : request.positions)
        {
            positions.push_back(EvaluationPosition{{position[0], position[1], position[2], 0.0f}});
        }

        EvaluationBuffers buffers;
        buffers.create(m_context->getDevice(), positions.size(), request.parameterValues.size());
        buffers.write(m_context->getQueue(),
                      EvaluationUniforms{request.isoValue,
                                         static_cast<std::uint32_t>(positions.size()),
                                         0u,
                                         0u},
                      positions,
                      request.parameterValues);

        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
        wgpu::ShaderModuleDescriptor shaderDescriptor;
        shaderDescriptor.nextInChain = &wgsl;
        auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

        wgpu::BindGroupLayoutEntry bindings[4]{};
        bindings[0].binding = 0u;
        bindings[0].visibility = wgpu::ShaderStage::Compute;
        bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
        bindings[0].buffer.minBindingSize = sizeof(EvaluationUniforms);
        bindings[1].binding = 1u;
        bindings[1].visibility = wgpu::ShaderStage::Compute;
        bindings[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
        bindings[1].buffer.minBindingSize = positions.size() * sizeof(EvaluationPosition);
        bindings[2].binding = 2u;
        bindings[2].visibility = wgpu::ShaderStage::Compute;
        bindings[2].buffer.type = wgpu::BufferBindingType::Storage;
        bindings[2].buffer.minBindingSize = buffers.outputSize();
        bindings[3].binding = 3u;
        bindings[3].visibility = wgpu::ShaderStage::Compute;
        bindings[3].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
        bindings[3].buffer.minBindingSize = sizeof(float);

        wgpu::BindGroupLayoutDescriptor layoutDescriptor;
        layoutDescriptor.entryCount = std::size(bindings);
        layoutDescriptor.entries = bindings;
        auto const bindGroupLayout = m_context->getDevice().CreateBindGroupLayout(&layoutDescriptor);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
        pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
        pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
        auto const pipelineLayout = m_context->getDevice().CreatePipelineLayout(&pipelineLayoutDescriptor);

        wgpu::ComputePipelineDescriptor pipelineDescriptor;
        pipelineDescriptor.layout = pipelineLayout;
        pipelineDescriptor.compute.module = shader;
        pipelineDescriptor.compute.entryPoint = "main";
        auto const pipeline = m_context->getDevice().CreateComputePipeline(&pipelineDescriptor);

        wgpu::BindGroupEntry bindGroupEntries[4]{};
        bindGroupEntries[0].binding = 0u;
        bindGroupEntries[0].buffer = buffers.uniform();
        bindGroupEntries[0].size = sizeof(EvaluationUniforms);
        bindGroupEntries[1].binding = 1u;
        bindGroupEntries[1].buffer = buffers.positions();
        bindGroupEntries[1].size = positions.size() * sizeof(EvaluationPosition);
        bindGroupEntries[2].binding = 2u;
        bindGroupEntries[2].buffer = buffers.output();
        bindGroupEntries[2].size = buffers.outputSize();
        bindGroupEntries[3].binding = 3u;
        bindGroupEntries[3].buffer = buffers.parameters();
        bindGroupEntries[3].size = buffers.parameterSize();

        wgpu::BindGroupDescriptor bindGroupDescriptor;
        bindGroupDescriptor.layout = bindGroupLayout;
        bindGroupDescriptor.entryCount = std::size(bindGroupEntries);
        bindGroupDescriptor.entries = bindGroupEntries;
        auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

        auto const workgroupCount = (static_cast<std::uint32_t>(positions.size()) + WORKGROUP_SIZE - 1u) /
                                     WORKGROUP_SIZE;
        auto const encoder = m_context->getDevice().CreateCommandEncoder();
        auto const computePass = encoder.BeginComputePass();
        computePass.SetPipeline(pipeline);
        computePass.SetBindGroup(0u, bindGroup);
        computePass.DispatchWorkgroups(workgroupCount);
        computePass.End();
        encoder.CopyBufferToBuffer(buffers.output(),
                                   0u,
                                   buffers.staging(),
                                   0u,
                                   buffers.outputSize());
        auto const commandBuffer = encoder.Finish();
        m_context->getQueue().Submit(1u, &commandBuffer);

        std::vector<float> values(positions.size());
        bool complete = false;
        std::string errorMessage;
        buffers.staging().MapAsync(
          wgpu::MapMode::Read,
          0u,
          buffers.outputSize(),
          wgpu::CallbackMode::AllowProcessEvents,
          [&values, &buffers, &complete, &errorMessage](wgpu::MapAsyncStatus const status,
                                                        wgpu::StringView const message)
          {
              (void)message;
              if (status != wgpu::MapAsyncStatus::Success)
              {
                  errorMessage = "WebGPU SDF evaluation readback failed";
                  complete = true;
                  return;
              }

              auto const * mappedValues = static_cast<float const *>(
                buffers.staging().GetConstMappedRange(0u, buffers.outputSize()));
              if (mappedValues == nullptr)
              {
                  errorMessage = "WebGPU SDF evaluation staging buffer returned no mapped data";
                  complete = true;
                  return;
              }

              std::memcpy(values.data(), mappedValues, buffers.outputSize());
              buffers.staging().Unmap();
              complete = true;
          });

        while (!complete)
        {
            m_context->processEvents();
            if (!m_context->isValid())
            {
                throw std::runtime_error(m_context->getErrorMessage());
            }
        }
        if (!errorMessage.empty())
        {
            throw std::runtime_error(errorMessage);
        }

        return compute::SdfEvaluationResult{.values = std::move(values)};
    }
}
