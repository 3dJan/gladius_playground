#include "webgpu/WebGPUProgramValidator.h"

#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/RenderContracts.h"
#include "nodes/Model.h"
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#ifdef Always
#undef Always
#endif

#include <webgpu/webgpu_cpp.h>

#include <atomic>
#include <stdexcept>
#include <utility>

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

        /// @brief State for the PopErrorScope callback, kept alive until it fires.
        struct ErrorScopeState
        {
            std::shared_ptr<WebGPUComputeContext> context;
            std::atomic<bool> complete{false};
            wgpu::ErrorType errorType{wgpu::ErrorType::NoError};
            std::string message;
        };
    }

    WebGPUProgramValidator::WebGPUProgramValidator(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(std::move(context))
    {
    }

    bool WebGPUProgramValidator::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    std::string WebGPUProgramValidator::getErrorMessage() const
    {
        return m_context ? m_context->getErrorMessage() : std::string{"No WebGPU context available"};
    }

    ProgramValidationResult WebGPUProgramValidator::validateModel(
      nodes::Model & model,
      ResourceManager const & resourceManager,
      std::uint64_t const sceneGeneration)
    {
        if (!isAvailable())
        {
            return {.ok = false, .message = getErrorMessage()};
        }

        compute::RenderSceneSnapshot snapshot;
        try
        {
            snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
              model, sceneGeneration, resourceManager);
        }
        catch (std::exception const & ex)
        {
            return {.ok = false, .message = ex.what()};
        }

        if (snapshot.analyticEvaluatorWgsl.empty())
        {
            return {.ok = false,
                    .message = "The model produced no WGSL evaluator; the graph may not reach an "
                               "output the analytic backend can express."};
        }

        auto const hasMeshes = !snapshot.meshResources.empty();
        auto const hasBeams = !snapshot.beamLatticeResources.empty();
        auto const hasImages = !snapshot.imageResources.empty();

        std::string shaderSource;
        try
        {
            shaderSource = WebGPUSdfShaderComposer::composeColorWithResourceSupport(
              snapshot.analyticEvaluatorWgsl, hasMeshes, hasBeams, hasImages);
        }
        catch (std::exception const & ex)
        {
            return {.ok = false, .message = ex.what()};
        }

        auto const result = validateShaderSource(shaderSource);
        return {.ok = result.ok, .message = result.message, .shaderSource = std::move(shaderSource)};
    }

    ProgramValidationResult WebGPUProgramValidator::validateShaderSource(std::string const & shaderSource)
    {
        if (!isAvailable())
        {
            return {.ok = false, .message = getErrorMessage()};
        }
#ifdef __EMSCRIPTEN__
        // Popping a validation error scope needs the host event loop, which this synchronous
        // entry point cannot pump. The browser build compiles shaders as part of rendering.
        (void)shaderSource;
        return {.ok = false,
                .message = "Blocking WebGPU program validation is not available in the browser build."};
#else
        WebGPUComputeContext::DeviceLock const lock(*m_context);
        if (!m_context->isValid())
        {
            return {.ok = false, .message = m_context->getErrorMessage()};
        }

        auto const device = m_context->getDevice();
        device.PushErrorScope(wgpu::ErrorFilter::Validation);

        wgpu::ShaderModule shader{};
        std::string creationError;
        try
        {
            wgpu::ShaderSourceWGSL wgsl;
            wgsl.code = {shaderSource.data(), shaderSource.size()};
            wgpu::ShaderModuleDescriptor descriptor;
            descriptor.nextInChain = &wgsl;
            shader = device.CreateShaderModule(&descriptor);
        }
        catch (std::exception const & ex)
        {
            creationError = ex.what();
        }

        auto state = std::make_shared<ErrorScopeState>();
        state->context = m_context;
        device.PopErrorScope(
#ifdef __EMSCRIPTEN__
          wgpu::CallbackMode::AllowSpontaneous,
#else
          wgpu::CallbackMode::AllowProcessEvents,
#endif
          [state](wgpu::PopErrorScopeStatus const status,
                  wgpu::ErrorType const type,
                  wgpu::StringView const message)
          {
              WebGPUComputeContext::DeviceLock const callbackLock(*state->context);
              if (status != wgpu::PopErrorScopeStatus::Success)
              {
                  state->errorType = wgpu::ErrorType::Unknown;
                  state->message = "Failed to pop the WebGPU validation error scope";
              }
              else
              {
                  state->errorType = type;
                  state->message = toString(message);
              }
              state->complete.store(true, std::memory_order_release);
          });

        // Dawn fires the callback from ProcessEvents on this thread; the context gate is a
        // recursive mutex, so the callback's own lock re-enters safely.
        while (!state->complete.load(std::memory_order_acquire) && m_context->isValid())
        {
            m_context->processEvents();
        }

        if (!state->complete.load(std::memory_order_acquire))
        {
            return {.ok = false, .message = m_context->getErrorMessage()};
        }

        shader = nullptr;

        if (!creationError.empty())
        {
            return {.ok = false, .message = creationError};
        }
        if (state->errorType != wgpu::ErrorType::NoError)
        {
            return {.ok = false,
                    .message = state->message.empty() ? "WebGPU program validation failed"
                                                      : std::move(state->message)};
        }
        return {.ok = true};
#endif
    }
}
