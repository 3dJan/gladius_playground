#include "compute/OpenCLComputeRenderer.h"

#include "ImageRGBA.h"
#include "RenderProgram.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gladius::compute
{
    namespace
    {
        class OpenCLRenderScene final : public IRenderScene
        {
          public:
            OpenCLRenderScene(SharedRenderSession session, std::uint64_t sceneGeneration)
                : m_session{std::move(session)}
                , m_sceneGeneration{sceneGeneration}
            {
            }

            [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
            {
                return ComputeBackendKind::OpenCL;
            }

            [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept override
            {
                return m_sceneGeneration;
            }

            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                return RendererCapability::AnalyticRendering | RendererCapability::ProgressiveRendering |
                       RendererCapability::FramePresentation;
            }

            [[nodiscard]] SharedRenderSession const & getSession() const noexcept
            {
                return m_session;
            }

          private:
            SharedRenderSession m_session;
            std::uint64_t m_sceneGeneration;
        };

        [[nodiscard]] RenderingSettings createOpenCLSettings(RenderSettingsSnapshot const & snapshot)
        {
            RenderingSettings settings{};
            settings.time_s = snapshot.timeSeconds;
            settings.z_mm = snapshot.sliceHeight;
            settings.flags = static_cast<int>(snapshot.flags);
            settings.approximation = snapshot.mode == RenderMode::Hybrid ? AM_HYBRID : AM_FULL_MODEL;
            settings.quality = snapshot.quality;
            settings.weightDistToNb = snapshot.weightDistanceToNeighbor;
            settings.weightMidPoint = snapshot.weightMidpoint;
            settings.normalOffset = snapshot.normalOffset;
            settings.earlyExitDistanceSq = snapshot.meshEarlyExitDistanceSquared;
            settings.meshInflationDistance = snapshot.meshInflationDistance;
            settings.meshFwnBeta = snapshot.meshFwnBeta;
            settings.meshFwnFarFieldFactor = snapshot.meshFwnFarFieldFactor;
            return settings;
        }

        [[nodiscard]] RenderSessionInputs createOpenCLInputs(RenderRequest const & request)
        {
            cl_float16 matrix{};
            matrix.s0 = request.camera.rightDirection[0];
            matrix.s1 = request.camera.rightDirection[1];
            matrix.s2 = request.camera.rightDirection[2];
            matrix.s4 = request.camera.upDirection[0];
            matrix.s5 = request.camera.upDirection[1];
            matrix.s6 = request.camera.upDirection[2];
            matrix.s8 = request.camera.forwardDirection[0];
            matrix.s9 = request.camera.forwardDirection[1];
            matrix.sa = request.camera.forwardDirection[2];

            return {.settings = createOpenCLSettings(request.settings),
                    .eyePosition = {request.camera.eyePosition[0],
                                    request.camera.eyePosition[1],
                                    request.camera.eyePosition[2]},
                    .modelViewPerspectiveMat = matrix};
        }

        [[nodiscard]] std::uint32_t packRgba8(cl_float4 const & color)
        {
            auto const toChannel = [](float value) {
                return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            };
            return toChannel(color.s[0]) | (toChannel(color.s[1]) << 8u) | (toChannel(color.s[2]) << 16u) |
                   (toChannel(color.s[3]) << 24u);
        }

        class OpenCLRenderSubmission final : public IRenderSubmission
        {
          public:
            OpenCLRenderSubmission(SharedRenderSession session,
                                   std::shared_ptr<ImageRGBA> target,
                                   cl::Event completionEvent,
                                   RenderRequest request)
                : m_session{std::move(session)}
                , m_target{std::move(target)}
                , m_completionEvent{std::move(completionEvent)}
                , m_request{std::move(request)}
            {
            }

            [[nodiscard]] RenderSubmissionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_mutex};
                return updateStatusLocked();
            }

            void requestCancellation() noexcept override
            {
                m_cancelRequested.store(true, std::memory_order_release);
            }

            void wait() override
            {
                std::scoped_lock lock{m_mutex};
                if (isTerminalLocked())
                {
                    return;
                }

                try
                {
                    m_completionEvent.wait();
                    materializeFrameLocked();
                }
                catch (std::exception const & error)
                {
                    m_errorMessage = error.what();
                    m_status = RenderSubmissionStatus::Failed;
                }
            }

            [[nodiscard]] std::optional<RenderFrame> takeFrame() override
            {
                std::scoped_lock lock{m_mutex};
                if (updateStatusLocked() == RenderSubmissionStatus::Pending)
                {
                    return std::nullopt;
                }
                return std::exchange(m_frame, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_mutex};
                return m_errorMessage;
            }

          private:
            [[nodiscard]] bool isTerminalLocked() const noexcept
            {
                return m_status == RenderSubmissionStatus::Succeeded || m_status == RenderSubmissionStatus::Cancelled ||
                       m_status == RenderSubmissionStatus::Failed;
            }

            [[nodiscard]] RenderSubmissionStatus updateStatusLocked() const noexcept
            {
                if (isTerminalLocked())
                {
                    return m_status;
                }

                try
                {
                    auto const executionStatus = m_completionEvent.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
                    if (executionStatus == CL_COMPLETE)
                    {
                        if (m_cancelRequested.load(std::memory_order_acquire))
                        {
                            m_status = RenderSubmissionStatus::Cancelled;
                        }
                        else
                        {
                            materializeFrameLocked();
                        }
                    }
                    else if (executionStatus < 0)
                    {
                        m_errorMessage = "OpenCL render submission failed";
                        m_status = RenderSubmissionStatus::Failed;
                    }
                }
                catch (std::exception const & error)
                {
                    m_errorMessage = error.what();
                    m_status = RenderSubmissionStatus::Failed;
                }
                return m_status;
            }

            void materializeFrameLocked() const
            {
                if (m_cancelRequested.load(std::memory_order_acquire))
                {
                    m_status = RenderSubmissionStatus::Cancelled;
                    return;
                }

                m_target->read();
                auto const & sourcePixels = m_target->getData();
                std::vector<std::uint32_t> pixels;
                auto const firstPixel = static_cast<std::size_t>(m_request.viewport.firstRow) * m_request.viewport.width;
                auto const pixelCount = m_request.viewport.pixelCount();
                pixels.reserve(pixelCount);
                std::transform(sourcePixels.begin() + static_cast<std::ptrdiff_t>(firstPixel),
                               sourcePixels.begin() + static_cast<std::ptrdiff_t>(firstPixel + pixelCount),
                               std::back_inserter(pixels),
                               packRgba8);

                m_frame = RenderFrame{.width = m_request.viewport.width,
                                      .height = m_request.viewport.height,
                                      .firstRow = m_request.viewport.firstRow,
                                      .endRow = m_request.viewport.endRow,
                                      .freshness = m_request.freshness,
                                      .pixels = std::move(pixels)};
                m_status = RenderSubmissionStatus::Succeeded;
            }

            SharedRenderSession m_session;
            std::shared_ptr<ImageRGBA> m_target;
            cl::Event m_completionEvent;
            RenderRequest m_request;
            mutable std::mutex m_mutex;
            std::atomic_bool m_cancelRequested{false};
            mutable RenderSubmissionStatus m_status{RenderSubmissionStatus::Pending};
            mutable std::optional<RenderFrame> m_frame;
            mutable std::string m_errorMessage;
        };
    }

    OpenCLComputeRenderer::OpenCLComputeRenderer(SharedRenderSession session)
        : m_session{std::move(session)}
    {
        if (!m_session)
        {
            throw std::invalid_argument("OpenCL renderer requires a render session");
        }
    }

    ComputeBackendKind OpenCLComputeRenderer::getBackendKind() const noexcept
    {
        return ComputeBackendKind::OpenCL;
    }

    RendererCapability OpenCLComputeRenderer::getCapabilities() const noexcept
    {
        return RendererCapability::AnalyticRendering | RendererCapability::ProgressiveRendering |
               RendererCapability::FramePresentation;
    }

    bool OpenCLComputeRenderer::isAvailable() const noexcept
    {
        return m_session->getComputeContext()->isValid() && m_session->isPayloadCurrent();
    }

    std::unique_ptr<IRenderScene> OpenCLComputeRenderer::materializeScene(RenderSceneSnapshot snapshot)
    {
        if (!snapshot.isValid())
        {
            throw std::invalid_argument("OpenCL render scene snapshot is invalid");
        }
        if (!isAvailable())
        {
            throw std::runtime_error("OpenCL render session is unavailable or stale");
        }
        if (!hasCapability(getCapabilities(), snapshot.requiredCapabilities))
        {
            throw std::invalid_argument("OpenCL renderer does not support the scene capabilities");
        }

        return std::make_unique<OpenCLRenderScene>(m_session, snapshot.sceneGeneration);
    }

    std::unique_ptr<IRenderSubmission> OpenCLComputeRenderer::submitFrame(IRenderScene const & scene,
                                                                            RenderRequest request)
    {
        auto const * openCLScene = dynamic_cast<OpenCLRenderScene const *>(&scene);
        if (openCLScene == nullptr || openCLScene->getSession() != m_session)
        {
            throw std::invalid_argument("OpenCL renderer received a scene from another renderer");
        }

        return submitFrame(std::move(request));
    }

    std::unique_ptr<IRenderSubmission> OpenCLComputeRenderer::submitFrame(RenderRequest request)
    {
        if (!request.isValid())
        {
            throw std::invalid_argument("OpenCL render request is invalid");
        }
        if (request.settings.mode == RenderMode::PrecomputedSdf || request.settings.mode == RenderMode::DistanceInitialized)
        {
            throw std::invalid_argument("OpenCL renderer mode requires an unimplemented neutral payload");
        }
        if (!isAvailable())
        {
            throw std::runtime_error("OpenCL render session is unavailable or stale");
        }

        auto renderProgram = m_session->tryGetRenderProgram();
        if (!renderProgram.has_value() || *renderProgram == nullptr || (*renderProgram)->isCompilationInProgress())
        {
            throw std::runtime_error("OpenCL render program is not ready");
        }

        auto const context = m_session->getComputeContext();
        auto target = std::make_shared<ImageRGBA>(*context, request.viewport.width, request.viewport.height);
        target->allocateOnDevice();

        auto completionEvent = (*renderProgram)->renderSceneAsync(context->GetQueue(),
                                                                   *m_session->getPrimitives(),
                                                                   *target,
                                                                   createOpenCLInputs(request),
                                                                   request.viewport.firstRow,
                                                                   request.viewport.endRow);
        if (!completionEvent())
        {
            throw std::runtime_error("OpenCL render dispatch was not submitted");
        }

        context->GetQueue().flush();
        return std::make_unique<OpenCLRenderSubmission>(m_session, std::move(target), std::move(completionEvent), std::move(request));
    }
}