#include "webgpu/WebGPUComputeBackend.h"

#include "webgpu/WebGPUBufferSet.h"
#include "webgpu/WebGPUFrameBufferSet.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUSliceShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::uint32_t MAX_FRAME_DIMENSION = 4096u;
        // Keep the explicit allocation guard, but allow common desktop viewport sizes such as
        // 3685x1855 (approximately 6.8 million pixels).
        constexpr std::size_t MAX_FRAME_PIXELS = 10u * 1024u * 1024u;
        constexpr std::uint32_t MAX_FRAME_RAY_STEPS = 2048u;

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

        struct WebGPUSliceSubmissionState
        {
            mutable std::mutex mutex;
            WebGPUBufferSet buffers;
            compute::ComputeCompletionStatus status{compute::ComputeCompletionStatus::Pending};
            std::optional<compute::SliceResult> result;
            std::string errorMessage;
        };

        struct WebGPUFrameSubmissionState
        {
            mutable std::mutex mutex;
            WebGPUFrameBufferSet buffers;
            compute::ComputeCompletionStatus status{compute::ComputeCompletionStatus::Pending};
            std::optional<compute::FrameResult> result;
            std::string errorMessage;
        };

        class WebGPUSliceSubmission final : public compute::ISliceSubmission
        {
          public:
            WebGPUSliceSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::SliceRequest request)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
                , m_state(std::make_shared<WebGPUSliceSubmissionState>())
            {
                try
                {
                    submit(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = exception.what();
                }
            }

            ~WebGPUSliceSubmission() override
            {
#ifndef __EMSCRIPTEN__
                if (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
#endif
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->status;
            }

            void wait() override
            {
#ifdef __EMSCRIPTEN__
                // Browser callbacks are delivered by the event loop. Waiting here would block
                // that loop and prevent the readback callback from ever running.
                return;
#else
                while (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    m_context->processEvents();
                    if (!m_context->isValid())
                    {
                        std::scoped_lock lock{m_state->mutex};
                        if (m_state->status == compute::ComputeCompletionStatus::Pending)
                        {
                            m_state->status = compute::ComputeCompletionStatus::Failed;
                            m_state->errorMessage = m_context->getErrorMessage();
                        }
                    }
                }
#endif
            }

            [[nodiscard]] std::optional<compute::SliceResult> takeResult() override
            {
                std::scoped_lock lock{m_state->mutex};
                if (m_state->status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_state->status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_state->result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->errorMessage;
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
                // Serialize all device/queue access: Dawn instance processing and queue
                // submission must not run concurrently from multiple threads.
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);

                auto const dispatchSize = calculateSliceDispatchSize(request.width, request.height);
                if (!dispatchSize.has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU slice dimensions");
                }

                m_state->buffers.resize(m_context->getDevice(),
                                        request.width,
                                        request.height,
                                        request.parameterValues.size());
                m_state->buffers.writeUniforms(m_context->getQueue(),
                                               SliceUniforms{.sliceZ = request.sliceZ,
                                                             .width = request.width,
                                                             .height = request.height,
                                                             .scale = request.scale});
                m_state->buffers.writeParameters(m_context->getQueue(), request.parameterValues);

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
                bindings[1].buffer.minBindingSize = m_state->buffers.getOutputSizeBytes();
                bindings[2].binding = 2u;
                bindings[2].visibility = wgpu::ShaderStage::Compute;
                bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[2].buffer.minBindingSize = sizeof(float);

                wgpu::BindGroupLayoutDescriptor layoutDescriptor;
                layoutDescriptor.entryCount = std::size(bindings);
                layoutDescriptor.entries = bindings;
                auto const bindGroupLayout =
                  m_context->getDevice().CreateBindGroupLayout(&layoutDescriptor);

                wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
                pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
                pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
                auto const pipelineLayout =
                  m_context->getDevice().CreatePipelineLayout(&pipelineLayoutDescriptor);

                wgpu::ComputePipelineDescriptor pipelineDescriptor;
                pipelineDescriptor.layout = pipelineLayout;
                pipelineDescriptor.compute.module = shader;
                pipelineDescriptor.compute.entryPoint = "main";
                auto const pipeline =
                  m_context->getDevice().CreateComputePipeline(&pipelineDescriptor);

                wgpu::BindGroupEntry bindGroupEntries[3]{};
                bindGroupEntries[0].binding = 0u;
                bindGroupEntries[0].buffer = m_state->buffers.getUniformBuffer();
                bindGroupEntries[0].size = sizeof(SliceUniforms);
                bindGroupEntries[1].binding = 1u;
                bindGroupEntries[1].buffer = m_state->buffers.getOutputBuffer();
                bindGroupEntries[1].size = m_state->buffers.getOutputSizeBytes();
                bindGroupEntries[2].binding = 2u;
                bindGroupEntries[2].buffer = m_state->buffers.getParameterBuffer();
                bindGroupEntries[2].size = m_state->buffers.getParameterSizeBytes();

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = bindGroupLayout;
                bindGroupDescriptor.entryCount = std::size(bindGroupEntries);
                bindGroupDescriptor.entries = bindGroupEntries;
                auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(pipeline);
                computePass.SetBindGroup(0u, bindGroup);
                computePass.DispatchWorkgroups(dispatchSize->workgroupsX,
                                               dispatchSize->workgroupsY);
                computePass.End();
                encoder.CopyBufferToBuffer(m_state->buffers.getOutputBuffer(),
                                           0u,
                                           m_state->buffers.getStagingBuffer(),
                                           0u,
                                           m_state->buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                                auto const state = m_state;
                                state->buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                                    state->buffers.getOutputSizeBytes(),
#ifdef __EMSCRIPTEN__
                                    wgpu::CallbackMode::AllowSpontaneous,
#else
                                    wgpu::CallbackMode::AllowProcessEvents,
#endif
                                    [state, width = m_width, height = m_height](wgpu::MapAsyncStatus const status,
                                                                                                                             wgpu::StringView const message)
                  {
                                            std::scoped_lock lock{state->mutex};
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU slice readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                                                state->buffers.getStagingBuffer().GetConstMappedRange(
                                                    0u, state->buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU slice staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount =
                                                state->buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                                            state->result = compute::SliceResult{.width = width,
                                                                                                                     .height = height,
                                                                                                                     .pixels = {mappedPixels,
                                                                                                                                            mappedPixels + pixelCount}};
                                            state->buffers.getStagingBuffer().Unmap();
                                            state->status = compute::ComputeCompletionStatus::Succeeded;
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            std::shared_ptr<WebGPUSliceSubmissionState> m_state;
        };

        class WebGPUFrameSubmission final : public compute::IFrameSubmission
        {
          public:
            WebGPUFrameSubmission(std::shared_ptr<WebGPUComputeContext> context,
                                  compute::FrameRequest request)
                : m_context(std::move(context))
                , m_width(request.width)
                , m_height(request.height)
                , m_state(std::make_shared<WebGPUFrameSubmissionState>())
            {
                try
                {
                    submit(std::move(request));
                }
                catch (std::exception const & exception)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = exception.what();
                }
            }

            ~WebGPUFrameSubmission() override
            {
#ifndef __EMSCRIPTEN__
                if (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
#endif
            }

            [[nodiscard]] compute::ComputeCompletionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->status;
            }

            void progress() noexcept override
            {
                if (getStatus() != compute::ComputeCompletionStatus::Pending || !m_context)
                {
                    return;
                }

                try
                {
#ifndef __EMSCRIPTEN__
                    m_context->processEvents();
#endif
                    if (!m_context->isValid())
                    {
                        std::scoped_lock lock{m_state->mutex};
                        if (m_state->status == compute::ComputeCompletionStatus::Pending)
                        {
                            m_state->status = compute::ComputeCompletionStatus::Failed;
                            m_state->errorMessage = m_context->getErrorMessage();
                        }
                    }
                }
                catch (std::exception const & error)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = error.what();
                }
                catch (...)
                {
                    std::scoped_lock lock{m_state->mutex};
                    m_state->status = compute::ComputeCompletionStatus::Failed;
                    m_state->errorMessage = "WebGPU event processing failed";
                }
            }

            void wait() override
            {
#ifdef __EMSCRIPTEN__
                // Browser callbacks are delivered by the event loop. Waiting here would block
                // that loop and prevent the readback callback from ever running.
                return;
#else
                while (getStatus() == compute::ComputeCompletionStatus::Pending)
                {
                    progress();
                }
#endif
            }

            [[nodiscard]] std::optional<compute::FrameResult> takeResult() override
            {
                std::scoped_lock lock{m_state->mutex};
                if (m_state->status != compute::ComputeCompletionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_state->status = compute::ComputeCompletionStatus::Failed;
                return std::exchange(m_state->result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_state->mutex};
                return m_state->errorMessage;
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
                return WebGPUFrameShaderComposer::compose(DEFAULT_EVALUATOR);
            }

            void submit(compute::FrameRequest request)
            {
                if (!m_context || !m_context->isValid())
                {
                    throw std::runtime_error("WebGPU compute context is unavailable");
                }
                // Serialize all device/queue access: Dawn instance processing and queue
                // submission must not run concurrently from multiple threads.
                WebGPUComputeContext::DeviceLock const deviceLock(*m_context);
                if (request.endRow == 0u)
                {
                    request.endRow = request.height;
                }
                if (request.firstRow >= request.endRow || request.endRow > request.height ||
                    !calculateSliceDispatchSize(request.width, request.endRow - request.firstRow)
                       .has_value())
                {
                    throw std::invalid_argument("Invalid WebGPU frame row range");
                }
                if (request.horizontalScale <= 0.0f || request.verticalScale <= 0.0f ||
                    request.maxRaySteps == 0u || request.maxTravelDistance <= 0.0f)
                {
                    throw std::invalid_argument("WebGPU frame camera values must be positive");
                }
                if (request.maxRaySteps > MAX_FRAME_RAY_STEPS)
                {
                    throw std::invalid_argument(
                      "WebGPU frame maxRaySteps exceeds the supported limit of 2048");
                }
                if (request.width > MAX_FRAME_DIMENSION || request.height > MAX_FRAME_DIMENSION)
                {
                    throw std::invalid_argument("WebGPU frame dimensions exceed the safe limit");
                }
                if (request.modelBounds.has_value() && !request.modelBounds->isValid())
                {
                    throw std::invalid_argument("WebGPU frame model bounds are invalid");
                }

                auto const framePixelCount = static_cast<std::size_t>(request.width) *
                                              static_cast<std::size_t>(request.height);
                if (framePixelCount > MAX_FRAME_PIXELS ||
                    framePixelCount / request.width != request.height)
                {
                    throw std::invalid_argument("WebGPU frame pixel count exceeds the safe limit");
                }

                auto const rowCount = request.endRow - request.firstRow;
                FrameUniforms uniforms{
                  .eyeAndMaxDistance = {request.eyePosition[0],
                                        request.eyePosition[1],
                                        request.eyePosition[2],
                                        request.maxTravelDistance},
                  .forwardAndHorizontalScale = {request.forwardDirection[0],
                                                request.forwardDirection[1],
                                                request.forwardDirection[2],
                                                request.horizontalScale},
                  .rightAndWidth = {request.rightDirection[0],
                                    request.rightDirection[1],
                                    request.rightDirection[2],
                                    static_cast<float>(request.width)},
                  .upAndHeight = {request.upDirection[0],
                                  request.upDirection[1],
                                  request.upDirection[2],
                                  static_cast<float>(request.height)},
                  .verticalScaleAndMaxSteps = {request.verticalScale,
                                               static_cast<float>(request.maxRaySteps),
                                               0.0f,
                                               0.0f},
                  .firstRowAndCount = {static_cast<float>(request.firstRow),
                                       static_cast<float>(rowCount),
                                       0.0f,
                                       0.0f},
                  .timeSliceQualityNormal = {request.timeSeconds,
                                             request.sliceHeight,
                                             request.quality,
                                             request.normalOffset},
                  .flagsModeReserved = {request.renderingFlags,
                                        request.renderingMode,
                                        request.modelBounds.has_value() ? 1u : 0u,
                                        0u},
                  .clippingBoxMin = {},
                  .clippingBoxMax = {}};
                if (request.modelBounds.has_value())
                {
                    uniforms.clippingBoxMin = {request.modelBounds->min[0],
                                               request.modelBounds->min[1],
                                               request.modelBounds->min[2],
                                               0.0f};
                    uniforms.clippingBoxMax = {request.modelBounds->max[0],
                                               request.modelBounds->max[1],
                                               request.modelBounds->max[2],
                                               0.0f};
                }
                m_state->buffers.resize(
                  m_context->getDevice(), request.width, rowCount, request.parameterValues.size());
                m_state->buffers.writeUniforms(
                  m_context->getQueue(),
                  uniforms);
                m_state->buffers.writeParameters(m_context->getQueue(), request.parameterValues);
                m_state->buffers.setMeshPayloads(m_context->getDevice(),
                                                 m_context->getQueue(),
                                                 request.meshPayloadTable);
                m_state->buffers.setBeamPayloads(m_context->getDevice(),
                                                 m_context->getQueue(),
                                                 request.beamPayloadTable);
                m_state->buffers.setImagePayloads(m_context->getDevice(),
                                                  m_context->getQueue(),
                                                  request.imagePayloadTable);

                if (request.shaderSource.empty())
                {
                    request.shaderSource = loadDefaultShader();
                }

                wgpu::ShaderSourceWGSL wgsl;
                wgsl.code = {request.shaderSource.data(), request.shaderSource.size()};
                wgpu::ShaderModuleDescriptor shaderDescriptor;
                shaderDescriptor.nextInChain = &wgsl;
                auto const shader = m_context->getDevice().CreateShaderModule(&shaderDescriptor);

                bool const hasMeshPayloads = m_state->buffers.hasMeshPayloads();
                bool const hasBeamPayloads = m_state->buffers.hasBeamPayloads();
                bool const hasImagePayloads = m_state->buffers.hasImagePayloads();
                wgpu::BindGroupLayoutEntry bindings[9]{};
                bindings[0].binding = 0u;
                bindings[0].visibility = wgpu::ShaderStage::Compute;
                bindings[0].buffer.type = wgpu::BufferBindingType::Uniform;
                bindings[0].buffer.minBindingSize = sizeof(FrameUniforms);
                bindings[1].binding = 1u;
                bindings[1].visibility = wgpu::ShaderStage::Compute;
                bindings[1].buffer.type = wgpu::BufferBindingType::Storage;
                bindings[1].buffer.minBindingSize = m_state->buffers.getOutputSizeBytes();
                bindings[2].binding = 2u;
                bindings[2].visibility = wgpu::ShaderStage::Compute;
                bindings[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                bindings[2].buffer.minBindingSize = sizeof(float);
                std::size_t bindingCount = 3u;
                if (hasMeshPayloads)
                {
                    bindings[bindingCount].binding = 4u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                    ++bindingCount;
                    bindings[bindingCount].binding = 5u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                    ++bindingCount;
                }
                if (hasBeamPayloads)
                {
                    bindings[bindingCount].binding = 6u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                    ++bindingCount;
                    bindings[bindingCount].binding = 7u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                    ++bindingCount;
                }
                if (hasImagePayloads)
                {
                    bindings[bindingCount].binding = 8u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = sizeof(float);
                    ++bindingCount;
                    bindings[bindingCount].binding = 9u;
                    bindings[bindingCount].visibility = wgpu::ShaderStage::Compute;
                    bindings[bindingCount].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
                    bindings[bindingCount].buffer.minBindingSize = 2u * sizeof(std::uint32_t);
                    ++bindingCount;
                }

                wgpu::BindGroupLayoutDescriptor layoutDescriptor;
                layoutDescriptor.entryCount = bindingCount;
                layoutDescriptor.entries = bindings;
                auto const bindGroupLayout =
                  m_context->getDevice().CreateBindGroupLayout(&layoutDescriptor);

                wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor;
                pipelineLayoutDescriptor.bindGroupLayoutCount = 1u;
                pipelineLayoutDescriptor.bindGroupLayouts = &bindGroupLayout;
                auto const pipelineLayout =
                  m_context->getDevice().CreatePipelineLayout(&pipelineLayoutDescriptor);

                wgpu::ComputePipelineDescriptor pipelineDescriptor;
                pipelineDescriptor.layout = pipelineLayout;
                pipelineDescriptor.compute.module = shader;
                pipelineDescriptor.compute.entryPoint = "main";
                auto const pipeline =
                  m_context->getDevice().CreateComputePipeline(&pipelineDescriptor);

                wgpu::BindGroupEntry bindGroupEntries[9]{};
                bindGroupEntries[0].binding = 0u;
                bindGroupEntries[0].buffer = m_state->buffers.getUniformBuffer();
                bindGroupEntries[0].size = sizeof(FrameUniforms);
                bindGroupEntries[1].binding = 1u;
                bindGroupEntries[1].buffer = m_state->buffers.getOutputBuffer();
                bindGroupEntries[1].size = m_state->buffers.getOutputSizeBytes();
                bindGroupEntries[2].binding = 2u;
                bindGroupEntries[2].buffer = m_state->buffers.getParameterBuffer();
                bindGroupEntries[2].size = m_state->buffers.getParameterSizeBytes();
                std::size_t bindGroupEntryCount = 3u;
                if (hasMeshPayloads)
                {
                    bindGroupEntries[bindGroupEntryCount].binding = 4u;
                                        bindGroupEntries[bindGroupEntryCount].buffer = m_state->buffers.getMeshPayloadBuffer();
                    bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getMeshPayloadBuffer().GetSize();
                    ++bindGroupEntryCount;
                    bindGroupEntries[bindGroupEntryCount].binding = 5u;
                                        bindGroupEntries[bindGroupEntryCount].buffer = m_state->buffers.getMeshOffsetTableBuffer();
                    bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getMeshOffsetTableBuffer().GetSize();
                    ++bindGroupEntryCount;
                }
                if (hasBeamPayloads)
                {
                    bindGroupEntries[bindGroupEntryCount].binding = 6u;
                                        bindGroupEntries[bindGroupEntryCount].buffer = m_state->buffers.getBeamPayloadBuffer();
                    bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getBeamPayloadBuffer().GetSize();
                    ++bindGroupEntryCount;
                    bindGroupEntries[bindGroupEntryCount].binding = 7u;
                                        bindGroupEntries[bindGroupEntryCount].buffer = m_state->buffers.getBeamOffsetTableBuffer();
                    bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getBeamOffsetTableBuffer().GetSize();
                    ++bindGroupEntryCount;
                }
                                if (hasImagePayloads)
                                {
                                        bindGroupEntries[bindGroupEntryCount].binding = 8u;
                                        bindGroupEntries[bindGroupEntryCount].buffer =
                                            m_state->buffers.getImagePayloadBuffer();
                                        bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getImagePayloadBuffer().GetSize();
                                        ++bindGroupEntryCount;
                                        bindGroupEntries[bindGroupEntryCount].binding = 9u;
                                        bindGroupEntries[bindGroupEntryCount].buffer =
                                            m_state->buffers.getImageOffsetTableBuffer();
                                        bindGroupEntries[bindGroupEntryCount].size =
                                            m_state->buffers.getImageOffsetTableBuffer().GetSize();
                                        ++bindGroupEntryCount;
                                }

                wgpu::BindGroupDescriptor bindGroupDescriptor;
                bindGroupDescriptor.layout = bindGroupLayout;
                bindGroupDescriptor.entryCount = bindGroupEntryCount;
                bindGroupDescriptor.entries = bindGroupEntries;
                auto const bindGroup = m_context->getDevice().CreateBindGroup(&bindGroupDescriptor);

                auto const dispatchSize = *calculateSliceDispatchSize(request.width, rowCount);
                auto const encoder = m_context->getDevice().CreateCommandEncoder();
                auto const computePass = encoder.BeginComputePass();
                computePass.SetPipeline(pipeline);
                computePass.SetBindGroup(0u, bindGroup);
                computePass.DispatchWorkgroups(dispatchSize.workgroupsX, dispatchSize.workgroupsY);
                computePass.End();
                encoder.CopyBufferToBuffer(m_state->buffers.getOutputBuffer(),
                                           0u,
                                           m_state->buffers.getStagingBuffer(),
                                           0u,
                                           m_state->buffers.getOutputSizeBytes());
                auto const commandBuffer = encoder.Finish();
                m_context->getQueue().Submit(1u, &commandBuffer);

                                auto const state = m_state;
                                state->buffers.getStagingBuffer().MapAsync(
                  wgpu::MapMode::Read,
                  0u,
                                    state->buffers.getOutputSizeBytes(),
#ifdef __EMSCRIPTEN__
                                    wgpu::CallbackMode::AllowSpontaneous,
#else
                                    wgpu::CallbackMode::AllowProcessEvents,
#endif
                                    [state, width = m_width, height = m_height](wgpu::MapAsyncStatus const status,
                                                                                                                             wgpu::StringView const message)
                  {
                                            std::scoped_lock lock{state->mutex};
                      if (status != wgpu::MapAsyncStatus::Success)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU frame readback failed: " + toString(message);
                          return;
                      }

                      auto const * mappedPixels = static_cast<std::uint32_t const *>(
                                                state->buffers.getStagingBuffer().GetConstMappedRange(
                                                    0u, state->buffers.getOutputSizeBytes()));
                      if (mappedPixels == nullptr)
                      {
                                                    state->status = compute::ComputeCompletionStatus::Failed;
                                                    state->errorMessage = "WebGPU frame staging buffer returned no mapped data";
                          return;
                      }

                      auto const pixelCount =
                                                state->buffers.getOutputSizeBytes() / sizeof(std::uint32_t);
                                            state->result = compute::FrameResult{.width = width,
                                                                                                                     .height = height,
                                                                                                                     .pixels = {mappedPixels,
                                                                                                                                            mappedPixels + pixelCount}};
                                            state->buffers.getStagingBuffer().Unmap();
                                            state->status = compute::ComputeCompletionStatus::Succeeded;
                  });
            }

            std::shared_ptr<WebGPUComputeContext> m_context;
            std::uint32_t m_width{};
            std::uint32_t m_height{};
            std::shared_ptr<WebGPUFrameSubmissionState> m_state;
        };
    }

    WebGPUComputeBackend::WebGPUComputeBackend(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(context ? std::move(context) : std::make_shared<WebGPUComputeContext>())
    {
    }

    compute::ComputeBackendKind WebGPUComputeBackend::getKind() const noexcept
    { return compute::ComputeBackendKind::WebGPU; }

    bool WebGPUComputeBackend::isAvailable() const noexcept
    { return m_context && m_context->isValid(); }

    std::unique_ptr<compute::ISliceSubmission>
    WebGPUComputeBackend::submitSlice(compute::SliceRequest request)
    { return std::make_unique<WebGPUSliceSubmission>(m_context, std::move(request)); }

    std::unique_ptr<compute::IFrameSubmission>
    WebGPUComputeBackend::submitFrame(compute::FrameRequest request)
    { return std::make_unique<WebGPUFrameSubmission>(m_context, std::move(request)); }
}
