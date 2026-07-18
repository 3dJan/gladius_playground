#include "webgpu/WebGPUComputeBackend.h"

#include "webgpu/WebGPUBufferSet.h"
#include "webgpu/WebGPUFrameBufferSet.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUSliceShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <cstring>
#include <stdexcept>
#include <utility>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    namespace
    {
        std::string toString(wgpu::StringView const value)
        {
            if (value.data == nullptr)
            {
                return {};
            }

            if (value.length == wgpu::kStrlen)
            {
                return std::string(value.data);
            }

            return std::string(value.data, value.length);
        }

        class WebGPUSliceSubmission final : public compute::ISliceSubmission
        {
          public:
            WebGPUSliceSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::SliceRequest request)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
            {
                try
                {
                    submit(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    m_status = compute::ComputeCompletionStatus::Failed;
                    m_errorMessage = exception.what();
                }
            }

            ~WebGPUSliceSubmission() override
            {
                if (m_status == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override
            {
                return m_status;
            }

            void wait() override
            {
                while (m_status == compute::ComputeCompletionStatus::Pending)
                {
                    m_context->processEvents();
                    if (!m_context->isValid())
                    {
                        m_status = compute::ComputeCompletionStatus::Failed;
                        m_errorMessage = m_context->getErrorMessage();
                    }
                }
            }

            [[nodiscard]] std::optional<compute::SliceResult> takeResult() override
            {
                if (m_status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return m_errorMessage;
            }

          private:
            static std::string loadDefaultShader()
            {
                constexpr std::string_view DEFAULT_EVALUATOR = R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    let distance = length(position) - 0.5;
    return vec4<f32>(vec3<f32>(1.0), distance);
}
)";
                return WebGPUSliceShaderComposer::compose(DEFAULT_EVALUATOR);
            }

            void submit(compute::SliceRequest request)
            {
                if (!m_context || !m_context->isValid())
                {
                    throw std::runtime_error("WebGPU compute context is unavailable");
                }

                auto const dispatchSize = calculateSliceDispatchSize(request.width, request.height);
                if (!dispatchSize.has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU slice dimensions");
                }

                m_buffers.resize(m_context->getDevice(),
                                 request.width,
                                 request.height,
                                 request.parameterValues.size());
                m_buffers.writeUniforms(
                  m_context->getQueue(),
                  SliceUniforms{.sliceZ = request.sliceZ,
                                .width = request.width,
                                .height = request.height,
                                .scale = request.scale});
                m_buffers.writeParameters(m_context->getQueue(), request.parameterValues);

                if (request.shaderSource.empty())
                {
                    request.shaderSource = loadDefaultShader();
                }

                wgpu::ShaderSourceWGSL wgsl;
                wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
                wgpu::ShaderModuleDescriptor shaderDescriptor;
                shaderDescriptor.nextInChain = &wgsl;
                auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

                wgpu::BindGroupLayoutEntry bindings[3]{};
                bindings[0].binding = 0u;
                bindings[0].visibility = wgpu::ShaderStage::Compute;
                bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
                bindings[0].buffer.minBindingSize = sizeof(SliceUniforms);
                bindings[1].binding = 1u;
                bindings[1].visibility = wgpu::ShaderStage::Compute;
                bindings[1].buffer.type = wgpu::BufferBindingType::Storage;
                bindings[1].buffer.minBindingSize = m_buffers.getOutputSizeBytes();
                bindings[2].binding = 2u;
                bindings[2].visibility = wgpu::ShaderStage::Compute;
                bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[2].buffer.minBindingSize = sizeof(float);

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

                wgpu::BindGroupEntry bindGroupEntries[3]{};
                bindGroupEntries[0].binding = 0u;
                bindGroupEntries[0].buffer = m_buffers.getUniformBuffer();
                bindGroupEntries[0].size = sizeof(SliceUniforms);
                bindGroupEntries[1].binding = 1u;
                bindGroupEntries[1].buffer = m_buffers.getOutputBuffer();
                bindGroupEntries[1].size = m_buffers.getOutputSizeBytes();
                bindGroupEntries[2].binding = 2u;
                bindGroupEntries[2].buffer = m_buffers.getParameterBuffer();
                bindGroupEntries[2].size = m_buffers.getParameterSizeBytes();

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = bindGroupLayout;
                bindGroupDescriptor.entryCount = std::size(bindGroupEntries);
                bindGroupDescriptor.entries = bindGroupEntries;
                auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(pipeline);
                computePass.SetBindGroup(0u, bindGroup);
                computePass.DispatchWorkgroups(dispatchSize->workgroupsX, dispatchSize->workgroupsY);
                computePass.End();
                encoder.CopyBufferToBuffer(m_buffers.getOutputBuffer(),
                                           0u,
                                           m_buffers.getStagingBuffer(),
                                           0u,
                                           m_buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                m_buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                  m_buffers.getOutputSizeBytes(),
                  wgpu::CallbackMode::AllowProcessEvents,
                  [this](wgpu::MapAsyncStatus const status, wgpu::StringView const message)
                  {
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                          m_status = compute::ComputeCompletionStatus::Failed;
                          m_errorMessage = "WebGPU slice readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                        m_buffers.getStagingBuffer().GetConstMappedRange(0u, m_buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                          m_status = compute::ComputeCompletionStatus::Failed;
                          m_errorMessage = "WebGPU slice staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount = m_buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                      m_result = compute::SliceResult{.width = m_width,
                                                      .height = m_height,
                                                      .pixels = {mappedPixels, mappedPixels + pixelCount}};
                      m_buffers.getStagingBuffer().Unmap();
                      m_status = compute::ComputeCompletionStatus::Succeeded;
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            WebGPUBufferSet m_buffers;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            compute::ComputeCompletionStatus m_status{compute::ComputeCompletionStatus::Pending};
            std::optional<compute::SliceResult> m_result;
            std::string m_errorMessage;
        };

        class WebGPUFrameSubmission final : public compute::IFrameSubmission
        {
          public:
            WebGPUFrameSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::FrameRequest request)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
            {
                try
                {
                    submit(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    m_status = compute::ComputeCompletionStatus::Failed;
                    m_errorMessage = exception.what();
                }
            }

            ~WebGPUFrameSubmission() override
            {
                if (m_status == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override { return m_status; }

            void wait() override
            {
                while (m_status == compute::ComputeCompletionStatus::Pending)
                {
                    m_context->processEvents();
                    if (!m_context->isValid())
                    {
                        m_status = compute::ComputeCompletionStatus::Failed;
                        m_errorMessage = m_context->getErrorMessage();
                    }
                }
            }

            [[nodiscard]] std::optional<compute::FrameResult> takeResult() override
            {
                if (m_status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override { return m_errorMessage; }

          private:
            static std::string loadDefaultShader()
            {
                constexpr std::string_view DEFAULT_EVALUATOR = R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    let distance = length(position) - 0.5;
    return vec4<f32>(vec3<f32>(1.0), distance);
}
)";
                return WebGPUFrameShaderComposer::compose(DEFAULT_EVALUATOR);
            }

            void submit(compute::FrameRequest request)
            {
                if (!m_context || !m_context->isValid())
                {
                    throw std::runtime_error("WebGPU compute context is unavailable");
                }
                if (!calculateSliceDispatchSize(request.width, request.height).has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU frame dimensions");
                }
                if (request.verticalFieldOfViewRadians <= 0.0f || request.maxDistance <= 0.0f)
                {
                    throw std::invalid_argument("WebGPU frame camera values must be positive");
                }

                m_buffers.resize(m_context->getDevice(), request.width, request.height, request.parameterValues.size());
                m_buffers.writeUniforms(
                  m_context->getQueue(),
                  FrameUniforms{.eyeAndMaxDistance = {request.eyePosition[0],
                                                      request.eyePosition[1],
                                                      request.eyePosition[2],
                                                      request.maxDistance},
                                .forwardAndFieldOfView = {request.forwardDirection[0],
                                                          request.forwardDirection[1],
                                                          request.forwardDirection[2],
                                                          request.verticalFieldOfViewRadians},
                                .rightAndWidth = {request.rightDirection[0],
                                                  request.rightDirection[1],
                                                  request.rightDirection[2],
                                                  static_cast<float>(request.width)},
                                .upAndHeight = {request.upDirection[0],
                                                request.upDirection[1],
                                                request.upDirection[2],
                                                static_cast<float>(request.height)}});
                m_buffers.writeParameters(m_context->getQueue(), request.parameterValues);

                if (request.shaderSource.empty())
                {
                    request.shaderSource = loadDefaultShader();
                }

                wgpu::ShaderSourceWGSL wgsl;
                wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
                wgpu::ShaderModuleDescriptor shaderDescriptor;
                shaderDescriptor.nextInChain = &wgsl;
                auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

                wgpu::BindGroupLayoutEntry bindings[3]{};
                bindings[0].binding = 0u;
                bindings[0].visibility = wgpu::ShaderStage::Compute;
                bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
                bindings[0].buffer.minBindingSize = sizeof(FrameUniforms);
                bindings[1].binding = 1u;
                bindings[1].visibility = wgpu::ShaderStage::Compute;
                bindings[1].buffer.type = wgpu::BufferBindingType::Storage;
                bindings[1].buffer.minBindingSize = m_buffers.getOutputSizeBytes();
                bindings[2].binding = 2u;
                bindings[2].visibility = wgpu::ShaderStage::Compute;
                bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[2].buffer.minBindingSize = sizeof(float);

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

                wgpu::BindGroupEntry bindGroupEntries[3]{};
                bindGroupEntries[0].binding = 0u;
                bindGroupEntries[0].buffer = m_buffers.getUniformBuffer();
                bindGroupEntries[0].size = sizeof(FrameUniforms);
                bindGroupEntries[1].binding = 1u;
                bindGroupEntries[1].buffer = m_buffers.getOutputBuffer();
                bindGroupEntries[1].size = m_buffers.getOutputSizeBytes();
                bindGroupEntries[2].binding = 2u;
                bindGroupEntries[2].buffer = m_buffers.getParameterBuffer();
                bindGroupEntries[2].size = m_buffers.getParameterSizeBytes();

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = bindGroupLayout;
                bindGroupDescriptor.entryCount = std::size(bindGroupEntries);
                bindGroupDescriptor.entries = bindGroupEntries;
                auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

                auto const dispatchSize = *calculateSliceDispatchSize(request.width, request.height);
                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(pipeline);
                computePass.SetBindGroup(0u, bindGroup);
                computePass.DispatchWorkgroups(dispatchSize.workgroupsX, dispatchSize.workgroupsY);
                computePass.End();
                encoder.CopyBufferToBuffer(m_buffers.getOutputBuffer(),
                                           0u,
                                           m_buffers.getStagingBuffer(),
                                           0u,
                                           m_buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                m_buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                  m_buffers.getOutputSizeBytes(),
                  wgpu::CallbackMode::AllowProcessEvents,
                  [this](wgpu::MapAsyncStatus const status, wgpu::StringView const message)
                  {
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                          m_status = compute::ComputeCompletionStatus::Failed;
                          m_errorMessage = "WebGPU frame readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                        m_buffers.getStagingBuffer().GetConstMappedRange(0u, m_buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                          m_status = compute::ComputeCompletionStatus::Failed;
                          m_errorMessage = "WebGPU frame staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount = m_buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                      m_result = compute::FrameResult{.width = m_width,
                                                      .height = m_height,
                                                      .pixels = {mappedPixels, mappedPixels + pixelCount}};
                      m_buffers.getStagingBuffer().Unmap();
                      m_status = compute::ComputeCompletionStatus::Succeeded;
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            WebGPUFrameBufferSet m_buffers;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            compute::ComputeCompletionStatus m_status{compute::ComputeCompletionStatus::Pending};
            std::optional<compute::FrameResult> m_result;
            std::string m_errorMessage;
        };
    }

    WebGPUComputeBackend::WebGPUComputeBackend()
        : m_context(std::make_shared<WebGPUComputeContext>())
    {
    }

    compute::ComputeBackendKind WebGPUComputeBackend::getKind() const noexcept
    {
        return compute::ComputeBackendKind::WebGPU;
    }

    bool WebGPUComputeBackend::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    std::unique_ptr<compute::ISliceSubmission>
    WebGPUComputeBackend::submitSlice(compute::SliceRequest request)
    {
        return std::make_unique<WebGPUSliceSubmission>(m_context, std::move(request));
    }

    std::unique_ptr<compute::IFrameSubmission>
    WebGPUComputeBackend::submitFrame(compute::FrameRequest request)
    {
        return std::make_unique<WebGPUFrameSubmission>(m_context, std::move(request));
    }
}
