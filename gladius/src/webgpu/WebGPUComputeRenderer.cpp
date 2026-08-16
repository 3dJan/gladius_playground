#include "webgpu/WebGPUComputeRenderer.h"

#include "webgpu/WebGPUFrameShaderComposer.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        class WebGPUAnalyticRenderScene final : public compute::IRenderScene
        {
          public:
            explicit WebGPUAnalyticRenderScene(compute::RenderSceneSnapshot snapshot)
                : m_snapshot{std::move(snapshot)}
            {
            }

            [[nodiscard]] compute::ComputeBackendKind getBackendKind() const noexcept override
            {
                return compute::ComputeBackendKind::WebGPU;
            }

            [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept override
            {
                return m_snapshot.sceneGeneration;
            }

            [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override
            {
                return m_snapshot.requiredCapabilities;
            }

            [[nodiscard]] compute::RenderSceneSnapshot const & getSnapshot() const noexcept
            {
                return m_snapshot;
            }

          private:
            compute::RenderSceneSnapshot m_snapshot;
        };

        class WebGPUFrameRenderSubmission final : public compute::IRenderSubmission
        {
          public:
            WebGPUFrameRenderSubmission(std::unique_ptr<compute::IFrameSubmission> submission,
                                        compute::RenderRequest request)
                : m_submission{std::move(submission)}
                , m_request{std::move(request)}
            {
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                if (!m_submission)
                {
                    return compute::RenderSubmissionStatus::Failed;
                }
                if (m_cancelRequested.load(std::memory_order_acquire) &&
                    m_submission->getStatus() != compute::ComputeCompletionStatus::Pending)
                {
                    return compute::RenderSubmissionStatus::Cancelled;
                }

                switch (m_submission->getStatus())
                {
                case compute::ComputeCompletionStatus::Pending:
                    return compute::RenderSubmissionStatus::Pending;
                case compute::ComputeCompletionStatus::Succeeded:
                    return compute::RenderSubmissionStatus::Succeeded;
                case compute::ComputeCompletionStatus::Failed:
                default:
                    return compute::RenderSubmissionStatus::Failed;
                }
            }

            void progress() noexcept override
            {
                if (m_submission)
                {
                    m_submission->progress();
                }
            }

            void requestCancellation() noexcept override
            {
                m_cancelRequested.store(true, std::memory_order_release);
            }

            void wait() override
            {
                if (m_submission)
                {
                    m_submission->wait();
                }
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                if (getStatus() != compute::RenderSubmissionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                auto result = m_submission->takeResult();
                if (!result.has_value())
                {
                    return std::nullopt;
                }

                return compute::RenderFrame{.width = result->width,
                                            .height = result->height,
                                            .firstRow = m_request.viewport.firstRow,
                                            .endRow = m_request.viewport.endRow,
                                            .freshness = m_request.freshness,
                                            .pixels = std::move(result->pixels)};
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return m_submission ? m_submission->getErrorMessage() : "WebGPU frame submission is unavailable";
            }

          private:
            std::unique_ptr<compute::IFrameSubmission> m_submission;
            compute::RenderRequest m_request;
            std::atomic_bool m_cancelRequested{false};
        };

        [[nodiscard]] compute::FrameRequest createFrameRequest(compute::RenderSceneSnapshot const & scene,
                                                               compute::RenderRequest const & request)
        {
            return {.width = request.viewport.width,
                    .height = request.viewport.height,
                    .firstRow = request.viewport.firstRow,
                    .endRow = request.viewport.endRow,
                    .eyePosition = request.camera.eyePosition,
                    .forwardDirection = request.camera.forwardDirection,
                    .rightDirection = request.camera.rightDirection,
                    .upDirection = request.camera.upDirection,
                    .horizontalScale = request.frustum.horizontalScale,
                    .verticalScale = request.frustum.verticalScale,
                    .maxRaySteps = request.settings.maxRaySteps,
                    .maxTravelDistance = request.settings.maxTravelDistance,
                    .timeSeconds = request.settings.timeSeconds,
                    .sliceHeight = request.settings.sliceHeight,
                    .quality = request.settings.quality,
                    .normalOffset = request.settings.normalOffset,
                    .renderingFlags = request.settings.flags,
                    .renderingMode = static_cast<std::uint32_t>(request.settings.mode),
                    .modelBounds = request.modelBounds,
                    .shaderSource = WebGPUFrameShaderComposer::compose(scene.analyticEvaluatorWgsl),
                    .parameterValues = scene.parameterValues};
        }
    }

    WebGPUComputeRenderer::WebGPUComputeRenderer()
        : m_backend{std::make_shared<WebGPUComputeBackend>()}
    {
    }

    compute::ComputeBackendKind WebGPUComputeRenderer::getBackendKind() const noexcept
    {
        return compute::ComputeBackendKind::WebGPU;
    }

    compute::RendererCapability WebGPUComputeRenderer::getCapabilities() const noexcept
    {
        return compute::RendererCapability::AnalyticRendering |
               compute::RendererCapability::ProgressiveRendering |
               compute::RendererCapability::LowResolutionPreview |
               compute::RendererCapability::FramePresentation;
    }

    bool WebGPUComputeRenderer::isAvailable() const noexcept
    {
        return m_backend && m_backend->isAvailable();
    }

    std::unique_ptr<compute::IRenderScene>
    WebGPUComputeRenderer::materializeScene(compute::RenderSceneSnapshot snapshot)
    {
        if (!isAvailable())
        {
            throw std::runtime_error("WebGPU renderer is unavailable");
        }
        if (!snapshot.isValid())
        {
            throw std::invalid_argument("WebGPU analytic scene snapshot is invalid");
        }
        if (!hasCapability(getCapabilities(), snapshot.requiredCapabilities))
        {
            throw std::invalid_argument("WebGPU renderer does not support the scene capabilities");
        }

        return std::make_unique<WebGPUAnalyticRenderScene>(std::move(snapshot));
    }

    std::unique_ptr<compute::IRenderSubmission>
    WebGPUComputeRenderer::submitFrame(compute::IRenderScene const & scene, compute::RenderRequest request)
    {
        if (!request.isValid())
        {
            throw std::invalid_argument("WebGPU render request is invalid");
        }
        auto const * analyticScene = dynamic_cast<WebGPUAnalyticRenderScene const *>(&scene);
        if (analyticScene == nullptr || analyticScene->getBackendKind() != getBackendKind())
        {
            throw std::invalid_argument("WebGPU renderer received a scene from another backend");
        }

        auto frameSubmission = m_backend->submitFrame(createFrameRequest(analyticScene->getSnapshot(), request));
        return std::make_unique<WebGPUFrameRenderSubmission>(std::move(frameSubmission), std::move(request));
    }

    std::unique_ptr<compute::IRenderSubmission> WebGPUComputeRenderer::submitFrame(compute::RenderRequest)
    {
        throw std::runtime_error("WebGPU renderer requires a materialized scene");
    }
}
