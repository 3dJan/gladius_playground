#include "RenderWindow.h"

#include <algorithm>
#include <cmath>
#include <coroutine>
#include <cstddef>
#include <fmt/core.h>
#include <iterator>

#include "../CLMath.h"
#include "../ComputeContext.h"
#include "../ConfigManager.h"
#include "../ContourExtractor.h"
#include "../Document.h"
#include "../IconFontCppHeaders/IconsFontAwesome5.h"
#include "../ImageRGBA.h"
#include "../TimeMeasurement.h"
#include "../io/MeshExporter.h"
#include "ExportState.h"
#include "GLView.h"
#include "Profiling.h"
#include "ShortcutManager.h"
#include "Widgets.h"
#include "OverflowMenuBar.h"
#include "compute/ComputeCore.h"
#include "imgui.h"
#include <nodes/Model.h>

namespace gladius::ui
{
    using namespace std;

    namespace
    {
        class ClEventAwaiter
        {
          public:
            explicit ClEventAwaiter(cl::Event event)
                : m_event(std::move(event))
            {
            }

            bool await_ready() const
            {
                if (!m_event())
                {
                    return true;
                }
                try
                {
                    auto const status = m_event.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>();
                    return status == CL_COMPLETE;
                }
                catch (...)
                {
                    return true;
                }
            }

            void await_suspend(std::coroutine_handle<> handle)
            {
                if (!m_event())
                {
                    handle.resume();
                    return;
                }

                auto * state = new CallbackState{handle};
                try
                {
                    m_event.setCallback(CL_COMPLETE, &ClEventAwaiter::onComplete, state);
                }
                catch (...)
                {
                    delete state;
                    throw;
                }
            }

            void await_resume() const noexcept
            {
            }

          private:
            struct CallbackState
            {
                std::coroutine_handle<> handle;
            };

            static void CL_CALLBACK onComplete(cl_event, cl_int, void * userData)
            {
                auto * state = static_cast<CallbackState *>(userData);
                auto handle = state->handle;
                delete state;
                handle.resume();
            }

            cl::Event m_event;
        };

        coro::task<void>
        waitForEvent(cl::Event event,
                     async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
        {
            if (!event() || (cancelCheck && cancelCheck()))
            {
                co_return;
            }

            co_await ClEventAwaiter(std::move(event));
        }

        constexpr size_t kInitialProgressiveStepSize = 8;
        constexpr float kAdaptivePreviewTargetFrameTimeMs = 25.0f;
        constexpr float kAdaptivePreviewMinErrorMs = 0.5f;
        constexpr float kAdaptivePreviewIntegralDecay = 0.8f;
        constexpr float kAdaptivePreviewProportionalGain = 0.001f;
        constexpr float kAdaptivePreviewIntegralGain = 0.00001f;
        constexpr float kAdaptivePreviewDerivativeGain = 0.000001f;
        constexpr float kAdaptivePreviewMinQuality = 0.05f;
        constexpr float kAdaptivePreviewMinDimension = 1.0f;
        constexpr float kAdaptivePreviewMaxDimension = 16000.0f;
        constexpr float kAdaptivePreviewResizeThresholdPercent = 5.0f;
    }

    void RenderWindow::initialize(ComputeCore * core,
                                  GLView * view,
                                  std::shared_ptr<ShortcutManager> shortcutManager,
                                  gladius::ConfigManager * configManager)
    {
        m_core = core;
        m_view = view;
        m_shortcutManager = shortcutManager;
        m_configManager = configManager;

        // Initialize settings when core is ready
        if (m_core && m_core->isRendererReady())
        {
            auto & settings = m_core->getResourceContext()->getRenderingSettings();
            m_renderWindowState.renderQuality = settings.quality;
            m_renderWindowState.renderQualityWhileMoving = settings.quality * 0.5f;
        }

        // Load permanent centering state from config
        if (m_configManager)
        {
            m_permanentCenteringEnabled =
              m_configManager->getValue<bool>("renderWindow", "permanentCenteringEnabled", false);
        }

        auto const realtimeConfig = loadRealtimeRaymarchConfig();
        m_renderUpdateCoordinator.configureRealtime(realtimeConfig);

        // Don't initialize async rendering here - will be done lazily on first render
    }

    void RenderWindow::setDocument(Document * doc)
    {
        m_document = doc;
    }

    void RenderWindow::setExportState(ExportState const * exportState)
    {
        m_exportState = exportState;
    }

    void RenderWindow::initializeAsyncRendering()
    {
        ProfileFunction;
        m_asyncConfig = async_rendering::loadAsyncRenderFeatureConfig(m_configManager);

        if (m_asyncConfig.wantsCoroutineBackend())
        {
            if (!m_asyncController)
            {
                m_asyncController = std::make_shared<async_rendering::AsyncRenderController>();
            }
            m_asyncController->setJobExecutor(
              [this](async_rendering::RenderJob const & job,
                     async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
                -> coro::task<async_rendering::FrameResultMeta>
              {
                  if (job.type == async_rendering::RenderJobType::BoundingBoxUpdate)
                  {
                      co_return co_await executeAsyncBboxUpdate(job, cancelCheck);
                  }
                  else if (job.type == async_rendering::RenderJobType::SDFPrecomputation)
                  {
                      co_return co_await executeAsyncSdfPrecomputation(job, cancelCheck);
                  }
                  else if (job.type == async_rendering::RenderJobType::ParameterUpdate)
                  {
                      co_return co_await executeAsyncParameterUpdate(job, cancelCheck);
                  }
                  else if (job.type == async_rendering::RenderJobType::LowResPreview)
                  {
                      co_return co_await executeAsyncPreviewJob(job, cancelCheck);
                  }
                  else if (job.type == async_rendering::RenderJobType::StreamingPreview)
                  {
                      co_return co_await executeStreamingPreviewJob(job, cancelCheck);
                  }

                  co_return co_await executeAsyncRenderJob(job, cancelCheck);
              });
            m_asyncController->start();
            m_asyncEpochCounter.store(0, std::memory_order_release);
            m_asyncCurrentEpoch.store(0, std::memory_order_release);
            m_asyncViewEpoch.store(0, std::memory_order_release);
            m_asyncInFlightEpoch.store(0, std::memory_order_release);
            m_asyncInFlightViewEpoch.store(0, std::memory_order_release);
            m_asyncFrameCounter.store(0, std::memory_order_release);
            m_asyncJobInFlight.store(false, std::memory_order_release);
            m_asyncRealtimeJobInFlight.store(false, std::memory_order_release);
            m_asyncSdfJobInFlight.store(false, std::memory_order_release);
            m_asyncSdfInFlightEpoch.store(0, std::memory_order_release);
            m_renderUpdateCoordinator = async_rendering::RenderUpdateCoordinator{};
            m_renderUpdateCoordinator.configureRealtime(loadRealtimeRaymarchConfig());
            m_pendingRenderCommands.clear();
            notifyAsyncEpochIncrement();
        }
        else
        {
            if (m_asyncController)
            {
                m_asyncController->stop();
                m_asyncController.reset();
            }
            m_asyncEpochCounter.store(0, std::memory_order_release);
            m_asyncCurrentEpoch.store(0, std::memory_order_release);
            m_asyncViewEpoch.store(0, std::memory_order_release);
            m_asyncInFlightEpoch.store(0, std::memory_order_release);
            m_asyncInFlightViewEpoch.store(0, std::memory_order_release);
            m_asyncFrameCounter.store(0, std::memory_order_release);
            m_asyncJobInFlight.store(false, std::memory_order_release);
            m_asyncRealtimeJobInFlight.store(false, std::memory_order_release);
            m_asyncSdfJobInFlight.store(false, std::memory_order_release);
            m_asyncSdfInFlightEpoch.store(0, std::memory_order_release);
        }
    }

    void RenderWindow::notifyAsyncEpochIncrement()
    {
        ProfileFunction;
        if (!m_asyncConfig.wantsCoroutineBackend())
        {
            return;
        }

        auto const oldEpoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        auto const newEpoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_asyncCurrentEpoch.store(newEpoch, std::memory_order_release);
        m_asyncViewEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_asyncInFlightEpoch.store(0, std::memory_order_release);
        m_asyncInFlightViewEpoch.store(0, std::memory_order_release);
        m_asyncJobInFlight.store(false, std::memory_order_release);
        m_asyncRealtimeJobInFlight.store(false, std::memory_order_release);
        m_asyncSdfJobInFlight.store(false, std::memory_order_release);
        m_asyncSdfInFlightEpoch.store(0, std::memory_order_release);
        m_asyncBboxJobInFlight.store(false, std::memory_order_release);
        
        // Invalidate distance init buffer on epoch change - camera/scene changed (FR-005)
        if (m_core)
        {
            m_core->invalidateDistanceInitBuffer();
        }

        // Release progressive buffer on epoch change
        if (m_asyncProgressiveBuffer)
        {
            [[maybe_unused]] bool const released =
              m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                     async_rendering::FrameState::Writing,
                                                     async_rendering::FrameState::Idle);
            m_asyncProgressiveBuffer = nullptr;
        }
        m_asyncProgressiveEpoch.store(0, std::memory_order_release);
        m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);

        // IMPORTANT: Release any other Writing buffers from old epoch
        // (they belong to cancelled jobs and should be returned to Idle)
        if (m_asyncController)
        {
            m_asyncController->releaseStaleBuffers(oldEpoch);
            m_asyncController->setLatestEpoch(newEpoch);
        }
    }

    void RenderWindow::invalidateCameraView()
    {
        ProfileFunction;
        m_dirty = true;
        m_renderWindowState.isMoving = true;
        [[maybe_unused]] bool const cameraChanged = m_camera.update(0.0f);
        m_renderWindowState.currentLine = 0;
        m_renderWindowState.renderingStepSize = kInitialProgressiveStepSize;
        m_renderWindowState.isRendering = false;
        bool const forceLowResPreview =
            m_renderUpdateCoordinator.realtimeConfig().mode == async_rendering::RealtimeRaymarchMode::Off;
        m_forceLowResRenderOnNextFrame.store(forceLowResPreview, std::memory_order_release);
        if (forceLowResPreview)
        {
            m_lastLowResRenderTime = std::chrono::system_clock::now();
        }
        else
        {
            m_lowResFeedbackPending.store(false, std::memory_order_release);
        }
        m_suppressHQDisplay.store(false, std::memory_order_release);
        m_asyncViewEpoch.fetch_add(1, std::memory_order_acq_rel);
        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraChanged());

        if (m_core)
        {
            m_core->invalidateDistanceInitBuffer();
        }

        if (m_asyncController && m_asyncProgressiveBuffer &&
            !m_asyncJobInFlight.load(std::memory_order_acquire))
        {
            [[maybe_unused]] bool const released =
              m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                     async_rendering::FrameState::Writing,
                                                     async_rendering::FrameState::Idle);
            m_asyncProgressiveBuffer = nullptr;
            m_asyncProgressiveEpoch.store(0, std::memory_order_release);
            m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
        }
    }

    async_rendering::RealtimeRaymarchConfig RenderWindow::loadRealtimeRaymarchConfig() const
    {
        async_rendering::RealtimeRaymarchConfig config{};
        if (!m_configManager)
        {
            return config;
        }

        auto const mode = m_configManager->getValue<std::string>(
          "renderWindow", "realtimeRaymarchMode", "auto");
        config.mode = async_rendering::realtimeRaymarchModeFromString(mode);
        config.targetFrameTimeMs =
          m_configManager->getValue<float>("renderWindow", "realtimeRaymarchTargetMs", 25.0f);
        return config;
    }

    void RenderWindow::saveRealtimeRaymarchMode(
      async_rendering::RealtimeRaymarchMode mode) const
    {
        if (!m_configManager)
        {
            return;
        }

        m_configManager->setValue("renderWindow",
                                  "realtimeRaymarchMode",
                                  std::string(async_rendering::realtimeRaymarchModeToString(mode)));
        m_configManager->save();
    }

    void RenderWindow::queueRenderDecision(async_rendering::RenderUpdateDecision decision)
    {
        m_pendingRenderCommands.insert(m_pendingRenderCommands.end(),
                                       std::make_move_iterator(decision.commands.begin()),
                                       std::make_move_iterator(decision.commands.end()));
    }

    bool RenderWindow::executeQueuedRenderCommands(RenderWindowState & state)
    {
        bool startedWork = false;
        size_t processedCommands = 0;
        while (!m_pendingRenderCommands.empty() && processedCommands < 64)
        {
            auto command = std::move(m_pendingRenderCommands.front());
            m_pendingRenderCommands.erase(m_pendingRenderCommands.begin());
            startedWork = executeRenderCommand(command, state) || startedWork;
            ++processedCommands;
        }
        return startedWork;
    }

    bool RenderWindow::executeRenderCommand(async_rendering::RenderCommand const & command,
                                            RenderWindowState & state)
    {
        if (command.type != async_rendering::RenderCommandType::StartTask)
        {
            return false;
        }

        return scheduleCoordinatorTask(command.task, state);
    }

    bool RenderWindow::scheduleCoordinatorTask(async_rendering::RenderTaskRequest const & task,
                                               RenderWindowState & state)
    {
        auto const failTask = [&]()
        { completeCoordinatorTask(task, async_rendering::RenderTaskStatus::Failed); };

        switch (task.type)
        {
        case async_rendering::RenderTaskType::ProgramCompilation:
            if (m_core && m_core->tryIsRenderProgramReady().value_or(false))
            {
                queueRenderDecision(m_renderUpdateCoordinator.notifyProgramCompilationCompleted());
            }
            return false;

        case async_rendering::RenderTaskType::ParameterUpload:
        {
            bool updated = false;
            if (m_core && m_document)
            {
                if (auto assembly = m_document->getAssembly(); assembly && m_core->isRendererReady())
                {
                    updated = m_core->tryToupdateParameter(*assembly);
                }
            }
            completeCoordinatorTask(task,
                                    updated ? async_rendering::RenderTaskStatus::Completed
                                            : async_rendering::RenderTaskStatus::Failed);
            return false;
        }

        case async_rendering::RenderTaskType::BoundingBoxUpdate:
        {
            bool const wasInFlight = m_asyncBboxJobInFlight.load(std::memory_order_acquire);
            scheduleAsyncBboxUpdate(&task);
            bool const started = !wasInFlight && m_asyncBboxJobInFlight.load(std::memory_order_acquire);
            if (!started)
            {
                failTask();
            }
            return started;
        }

        case async_rendering::RenderTaskType::SdfPrecomputation:
        {
            bool const started = scheduleAsyncSdfPrecomputation(&task);
            if (!started)
            {
                failTask();
            }
            return started;
        }

        case async_rendering::RenderTaskType::RealtimeFullFrame:
        {
            bool const started = scheduleRealtimeRenderJob(state, &task);
            if (!started)
            {
                m_renderUpdateCoordinator.recordRealtimeRejectedAttempt();
                failTask();
            }
            return started;
        }

        case async_rendering::RenderTaskType::LowResolutionPreview:
            if (isRealtimeRaymarchCameraInteraction())
            {
                completeCoordinatorTask(task, async_rendering::RenderTaskStatus::Cancelled);
                return false;
            }
            if (!m_core || !m_core->isSdfValid())
            {
                failTask();
                return false;
            }
        {
            bool const started = scheduleAsyncPreviewJob(&task);
            if (!started)
            {
                failTask();
            }
            return started;
        }

        case async_rendering::RenderTaskType::StreamingPreview:
            m_streamingPreviewActive.store(true, std::memory_order_release);
        {
            bool const started = scheduleStreamingPreviewJob(&task);
            if (!started)
            {
                failTask();
            }
            return started;
        }

        case async_rendering::RenderTaskType::ProgressiveHighQualityChunk:
            state.isRendering = true;
            if (!scheduleAsyncRenderJob(state, &task))
            {
                state.isRendering = false;
                failTask();
                return false;
            }
            return true;
        }

        return false;
    }

    bool RenderWindow::isRealtimeRaymarchCameraInteraction() const noexcept
    {
        return m_renderUpdateCoordinator.interactionState() ==
                 async_rendering::RenderInteractionState::CameraInteracting &&
               (m_renderUpdateCoordinator.isRealtimeActive() ||
                m_asyncRealtimeJobInFlight.load(std::memory_order_acquire));
    }

    bool RenderWindow::scheduleAsyncSdfPrecomputation(
      async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        if (!m_asyncController || !m_asyncController->isRunning() ||
            m_asyncSdfJobInFlight.load(std::memory_order_acquire))
        {
            return false;
        }

        async_rendering::RenderJob sdfJob{};
        sdfJob.type = async_rendering::RenderJobType::SDFPrecomputation;
        if (coordinatorTask != nullptr)
        {
            sdfJob.coordinatorRequestId = coordinatorTask->requestId;
            sdfJob.coordinatorStamp = coordinatorTask->stamp;
        }
        sdfJob.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (sdfJob.epoch == 0)
        {
            sdfJob.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_asyncCurrentEpoch.store(sdfJob.epoch, std::memory_order_release);
        }
        m_asyncController->setLatestEpoch(sdfJob.epoch);
        m_asyncSdfJobInFlight.store(true, std::memory_order_release);
        m_asyncSdfInFlightEpoch.store(sdfJob.epoch, std::memory_order_release);
        m_asyncController->enqueueJob(sdfJob);
        return true;
    }

    void RenderWindow::completeCoordinatorTask(async_rendering::RenderTaskRequest const & request,
                                               async_rendering::RenderTaskStatus status)
    {
        queueRenderDecision(m_renderUpdateCoordinator.completeTask(
          async_rendering::RenderTaskResult{.requestId = request.requestId,
                                            .type = request.type,
                                            .stamp = request.stamp,
                                            .status = status}));
    }

    void RenderWindow::completeCoordinatorTask(async_rendering::FrameResultMeta const & result,
                                               bool producedDisplayFrame)
    {
        if (result.coordinatorRequestId == 0)
        {
            return;
        }

        auto taskType = async_rendering::RenderTaskType::ProgressiveHighQualityChunk;
        switch (result.jobType)
        {
        case async_rendering::RenderJobType::RealtimeHighQuality:
            taskType = async_rendering::RenderTaskType::RealtimeFullFrame;
            break;
        case async_rendering::RenderJobType::LowResPreview:
            taskType = async_rendering::RenderTaskType::LowResolutionPreview;
            break;
        case async_rendering::RenderJobType::StreamingPreview:
            taskType = async_rendering::RenderTaskType::StreamingPreview;
            break;
        case async_rendering::RenderJobType::BoundingBoxUpdate:
            taskType = async_rendering::RenderTaskType::BoundingBoxUpdate;
            break;
        case async_rendering::RenderJobType::ParameterUpdate:
            taskType = async_rendering::RenderTaskType::ParameterUpload;
            break;
        case async_rendering::RenderJobType::SDFPrecomputation:
            taskType = async_rendering::RenderTaskType::SdfPrecomputation;
            break;
        case async_rendering::RenderJobType::ProgramCompilation:
            taskType = async_rendering::RenderTaskType::ProgramCompilation;
            break;
        case async_rendering::RenderJobType::HighQuality:
        default:
            taskType = async_rendering::RenderTaskType::ProgressiveHighQualityChunk;
            break;
        }

        auto const status = result.cancelled ? async_rendering::RenderTaskStatus::Cancelled
                                             : async_rendering::RenderTaskStatus::Completed;
        queueRenderDecision(m_renderUpdateCoordinator.completeTask(
          async_rendering::RenderTaskResult{.requestId = result.coordinatorRequestId,
                                            .type = taskType,
                                            .stamp = result.coordinatorStamp,
                                            .status = status,
                                            .durationNs = result.computeDurationNs,
                                            .producedDisplayFrame = producedDisplayFrame,
                                            .completedFrame = result.completedFrame}));
    }

    void RenderWindow::completeCoordinatorPreviewTask(async_rendering::PreviewResultMeta const & result)
    {
        if (result.coordinatorRequestId == 0)
        {
            return;
        }

        auto const status = result.cancelled ? async_rendering::RenderTaskStatus::Cancelled
                                             : async_rendering::RenderTaskStatus::Completed;
        queueRenderDecision(m_renderUpdateCoordinator.completeTask(
          async_rendering::RenderTaskResult{.requestId = result.coordinatorRequestId,
                                            .type = async_rendering::RenderTaskType::LowResolutionPreview,
                                            .stamp = result.coordinatorStamp,
                                            .status = status,
                                            .durationNs = result.latencyNs,
                                            .producedDisplayFrame = !result.cancelled,
                                            .completedFrame = !result.cancelled}));
    }

    void RenderWindow::renderWindow()
    {
        ProfileFunction;
        if (!m_isVisible)
        {
            return;
        }

        // Check if file loading is in progress. Once the interactive preview program is ready,
        // keep rendering instead of holding the preview area on the loading overlay while slower
        // post-load work continues in the background.
        bool const isFileLoading = m_document && m_document->isLoadingInProgress();
        auto const renderProgramReady = m_core ? m_core->tryIsRenderProgramReady()
                                               : std::optional<bool>{false};
        if (isFileLoading && !renderProgramReady.value_or(false))
        {
            renderLoadingOverlay();
            return;
        }

        // Execute rendering logic FIRST - this processes async results and promotes buffers.
        // Do NOT gate this on the compute token: processAsyncResults() and
        // processAsyncPreviewResults() must run every frame to consume completed async
        // jobs. Rendering functions that need the GPU acquire the token internally.
        render(m_renderWindowState);

        // THEN get the image to display - will see newly promoted front buffer
        std::shared_ptr<GLImageBuffer> displayImage;

        if (m_asyncConfig.wantsCoroutineBackend() && m_asyncController)
        {
            // Use the front buffer from triple buffering system
            auto * frontBuf = m_asyncController->frontBuffer();
            auto const currentEpoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
            auto const currentViewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);

            // For HQ frames (front buffer from progressive rendering) we require an epoch match
            // to avoid showing stale HQ results from an old parameter set.
            // For low-res preview during rapid parameter edits, we fall back to m_resultImage
            // which is updated synchronously and always shows *something* recent.
            // Exact realtime camera motion must not fall back to that preview texture.
            bool const realtimeCameraInteraction = isRealtimeRaymarchCameraInteraction();
            bool const realtimeRaymarchActive = m_renderUpdateCoordinator.isRealtimeActive();
            bool const epochMatches = frontBuf && frontBuf->epoch == currentEpoch;
            bool const viewMatches = frontBuf && frontBuf->viewEpoch == currentViewEpoch;
                        bool const allowRealtimeFront =
                            (realtimeCameraInteraction || realtimeRaymarchActive) && epochMatches;
            bool const frontBlockedByRendering = m_renderWindowState.isRendering &&
                                                 !allowRealtimeFront;
            bool const useFrontBuffer = frontBuf && frontBuf->image && epochMatches &&
                                        (viewMatches || allowRealtimeFront) &&
                                        !frontBlockedByRendering &&
                                        !m_suppressHQDisplay.load(std::memory_order_acquire);
                        bool const progressiveBufferCurrent =
                            m_asyncProgressiveEpoch.load(std::memory_order_acquire) == currentEpoch &&
                            m_asyncProgressiveViewEpoch.load(std::memory_order_acquire) == currentViewEpoch;
                        bool const useProgressiveBuffer =
                            m_asyncProgressiveBuffer && m_asyncProgressiveBuffer->image &&
                            progressiveBufferCurrent && !m_renderWindowState.isMoving &&
                            !m_suppressHQDisplay.load(std::memory_order_acquire);

            if (useFrontBuffer)
            {
                displayImage = frontBuf->image;
            }
            else if (useProgressiveBuffer)
            {
                displayImage = m_asyncProgressiveBuffer->image;
            }
            else
            {
                // Fallback to result image for progressive rendering or when no valid front buffer.
                // Preview result suppression keeps realtime camera motion from writing stale
                // surrogate frames here; returning nullptr would blank the viewport.
                displayImage = m_core->getResultImage();
            }
        }
        else
        {
            // Synchronous rendering - use result image
            displayImage = m_core->getResultImage();
        }

        if (!displayImage)
        {
            // No image yet (early init / between resize and re-allocation).
            // Skip drawing this frame instead of dereferencing a null shared_ptr.
            return;
        }

        displayImage->bind();
        auto const textureId = displayImage->GetTextureId();
        auto & io = ImGui::GetIO();
        ImGuiWindowFlags const window_flags =
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, window_flags);

        // Cache window state for isHovered() and isFocused() methods
        m_isWindowHovered = ImGui::IsWindowHovered();
        m_isWindowFocused = ImGui::IsWindowFocused();

        // if has focus, but not any item has focus, handle key input and content area is hovered
        if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemFocused() &&
            ImGui::IsMouseHoveringRect(m_contentAreaMin, m_contentAreaMax))
        {
            handleKeyInput();
        }

        if (ImGui::BeginMenuBar())
        {
            OverflowMenuBar overflow;
            overflow.begin("RenderWindowBar");

            overflow.item(ICON_FA_COMPRESS_ARROWS_ALT "\tCenter View",
                          [&]
                          {
                              if (ImGui::MenuItem(reinterpret_cast<const char *>(
                                    ICON_FA_COMPRESS_ARROWS_ALT "\tCenter View")))
                              {
                                  bool const asyncActive = isAsyncBackendActive();
                                  auto const bbox = tryFetchBoundingBox(true);
                                  if (bbox.has_value() || asyncActive)
                                  {
                                      centerView();
                                  }
                              }
                          });

            overflow.item(ICON_FA_CROSSHAIRS "\tPermanent Centering",
                          [&]
                          {
                              if (ImGui::MenuItem(reinterpret_cast<const char *>(
                                                    ICON_FA_CROSSHAIRS "\tPermanent Centering"),
                                                  nullptr,
                                                  m_permanentCenteringEnabled))
                              {
                                  togglePermanentCentering();
                              }

                              if (ImGui::IsItemHovered())
                              {
                                  std::string shortcutText = "No shortcut assigned";
                                  if (m_shortcutManager)
                                  {
                                      auto shortcut = m_shortcutManager->getShortcut(
                                        "camera.togglePermanentCentering");
                                      if (!shortcut.isEmpty())
                                      {
                                          shortcutText = shortcut.toString();
                                      }
                                  }

                                  ImGui::SetTooltip(
                                    "Automatically center view when model changes, camera moves, "
                                    "or "
                                    "viewport resizes\nShortcut: %s",
                                    shortcutText.c_str());
                              }
                          });

            overflow.item(ICON_FA_ROBOT "\tHQ",
                          [&]
                          {
                              toggleButton(
                                {reinterpret_cast<const char *>(ICON_FA_ROBOT "\tHQ")},
                                &m_enableHQRendering);
                          });

            overflow.item("Rendering Options",
                          [&]
                          {
                              int renderingFlags =
                                m_core->getResourceContext()->getRenderingSettings().flags;

                              bool flagsChanged = false;
                              if (ImGui::BeginMenu("..."))
                              {
                                  flagsChanged |= ImGui::CheckboxFlags(
                                    "Show Build Plate", &renderingFlags, RF_SHOW_BUILDPLATE);
                                  flagsChanged |= ImGui::CheckboxFlags(
                                    "Cut Off Object", &renderingFlags, RF_CUT_OFF_OBJECT);
                                  flagsChanged |= ImGui::CheckboxFlags(
                                    "Show Field", &renderingFlags, RF_SHOW_FIELD);
                                  flagsChanged |= ImGui::CheckboxFlags(
                                    "Show Stack", &renderingFlags, RF_SHOW_STACK);
                                  flagsChanged |= ImGui::CheckboxFlags(
                                    "Show Coordinate System",
                                    &renderingFlags,
                                    RF_SHOW_COORDINATE_SYSTEM);

                                  ImGui::Separator();
                                  float quality =
                                    m_core->getResourceContext()->getRenderingSettings().quality;
                                  ImGui::SetNextItemWidth(150.f * m_uiScale);
                                  bool qualityChanged =
                                    ImGui::SliderFloat("Quality", &quality, 0.1f, 2.0f);

                                  if (ImGui::IsItemHovered())
                                  {
                                      ImGui::SetTooltip(
                                        "Rendering quality (0.1 = Fast, 2.0 = Highest Quality)");
                                  }
                                  if (qualityChanged)
                                  {
                                      m_core->getResourceContext()->getRenderingSettings().quality =
                                        quality;
                                      m_renderWindowState.renderQuality = quality;
                                      m_renderWindowState.renderQualityWhileMoving = quality * 0.5f;
                                      invalidateView();
                                  }
                                  m_renderWindowState.renderQuality = quality;

                                  ImGui::Separator();
                                  if (ImGui::BeginMenu("Realtime Raymarch"))
                                  {
                                      auto config = m_renderUpdateCoordinator.realtimeConfig();
                                      auto const setMode = [&](async_rendering::RealtimeRaymarchMode mode)
                                      {
                                          config.mode = mode;
                                          m_renderUpdateCoordinator.configureRealtime(config);
                                          saveRealtimeRaymarchMode(mode);
                                      };

                                      if (ImGui::MenuItem(
                                            "Auto", nullptr, config.mode == async_rendering::RealtimeRaymarchMode::Auto))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Auto);
                                      }
                                      if (ImGui::MenuItem(
                                            "Off", nullptr, config.mode == async_rendering::RealtimeRaymarchMode::Off))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Off);
                                      }
                                      if (ImGui::MenuItem(
                                            "Force", nullptr, config.mode == async_rendering::RealtimeRaymarchMode::Force))
                                      {
                                          setMode(async_rendering::RealtimeRaymarchMode::Force);
                                      }

                                      ImGui::Separator();
                                      ImGui::Text("Budget: %.1f ms", config.targetFrameTimeMs);
                                      if (ImGui::IsItemHovered())
                                      {
                                          ImGui::SetTooltip(
                                            "Auto attempts full-resolution async raymarching when "
                                            "recent timings fit this budget; otherwise Gladius "
                                            "falls back to low-res preview/progressive rendering.");
                                      }
                                      ImGui::EndMenu();
                                  }

                                  ImGui::EndMenu();
                              }

                              if (flagsChanged)
                              {
                                  invalidateView();
                              }

                              m_core->getResourceContext()->getRenderingSettings().flags =
                                renderingFlags;
                          });

            overflow.end();

            // Compilation status indicator
            if (m_core->isAnyCompilationInProgressNonBlocking())
            {
                ImGui::TextUnformatted("Compilation in progress");
            }

            ImGui::EndMenuBar();
        }

        m_contentAreaMin = ImGui::GetWindowContentRegionMin();
        m_contentAreaMax = ImGui::GetWindowContentRegionMax();

        auto const prevRenderWindowSize = m_renderWindowSize_px;
        m_renderWindowSize_px = {
          {ImGui::GetWindowWidth(),
           ImGui::GetWindowContentRegionMax().y - ImGui::GetWindowContentRegionMin().y}};
        
        // Defensive check: ensure minimum viewport dimensions
        float constexpr minDimension = 1.0f;
        m_renderWindowSize_px.x = std::max(m_renderWindowSize_px.x, minDimension);
        m_renderWindowSize_px.y = std::max(m_renderWindowSize_px.y, minDimension);
        
        float constexpr tolerance = 1.E-4f;
        bool const sizeChanged =
          fabs(prevRenderWindowSize.x - m_renderWindowSize_px.x) > tolerance ||
          fabs(prevRenderWindowSize.y - m_renderWindowSize_px.y) > tolerance;
        if (sizeChanged)
        {
            // Preserve existing framebuffer content during resize to prevent flicker
            // Schedule low-res preview at new dimensions without clearing current display
            m_preserveContentDuringResize = true;
            m_deferredResizePending = true;
            m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
            m_lastLowResRenderTime = std::chrono::system_clock::now();
            m_dirty = true;

            // Mark viewport size as changed for permanent centering
            if (m_permanentCenteringEnabled)
            {
                m_viewportSizeChangedSinceLastCenter = true;
            }
        }
        else if (m_deferredResizePending)
        {
            // Size has stabilized — allow render() to proceed next frame and
            // reallocate GPU buffers to the new dimensions.
            m_deferredResizePending = false;
            m_preserveContentDuringResize = false;
        }

        ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(textureId)),
                     ImVec2(m_renderWindowSize_px.x, m_renderWindowSize_px.y));

        auto const contentMin =
          ImVec2{ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                 ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y};

        auto const contentMax =
          ImVec2{ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                 ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y};

        auto const windowCenter =
          ImVec2(0.5f * (contentMin.x + contentMax.x), 0.5f * (contentMin.y + contentMax.y));

        // Update camera and set isMoving based on whether camera is currently animating.
        // Note: Parameter changes use forceLowResRender and lowResFeedbackPending flags,
        // not isMoving. isMoving is specifically for camera movement.
        m_renderWindowState.isMoving = m_camera.update(io.DeltaTime * 1000.f);
        m_dirty = m_dirty || m_renderWindowState.isMoving;

        // Floating cut-height slider overlay (inside the Preview window)
        slider(contentMin, contentMax);

        // Event handling — skip camera input while slider widgets are being dragged
        bool const sliderActive = ImGui::IsAnyItemActive();
        if (ImGui::IsWindowHovered() && !sliderActive)
        {

            io.MouseDragThreshold = 1.f;
            auto mousePos = io.MousePos;

            if (m_camera.mouseMotionHandler(mousePos.x, mousePos.y))
            {
                invalidateCameraView();
            }
            if (!ImGui::IsAnyMouseDown())
            {
                m_camera.mouseInputHandler(ImGuiMouseButton_Left, -1, mousePos.x, mousePos.y);
            }

            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                ImGui::IsMouseHoveringRect(contentMin, contentMax))
            {
                m_camera.mouseInputHandler(ImGuiMouseButton_Left, 0, mousePos.x, mousePos.y);
            }
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
                ImGui::IsMouseHoveringRect(contentMin, contentMax))
            {
                m_camera.mouseInputHandler(ImGuiMouseButton_Right, 0, mousePos.x, mousePos.y);
            }
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
                ImGui::IsMouseHoveringRect(contentMin, contentMax))
            {
                m_camera.mouseInputHandler(ImGuiMouseButton_Middle, 0, mousePos.x, mousePos.y);
            }

            // Wheel zoom is handled via ShortcutManager (camera.zoomInWheel / camera.zoomOutWheel)
        }

        ImGui::End();
        ImGui::PopStyleVar();
        displayImage->unbind();

        // Show the progress indicator only when no renderable program is available.
        // In automatic mode the optimized OpenCL program may still compile in the
        // background while the command-stream preview program is already usable.
        bool const showBusyIndicator = !m_core->tryIsRenderProgramReady().value_or(false);
        if (showBusyIndicator)
        {
            m_view->startAnimationMode();
            ImGuiWindowFlags window_flags =
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
              ImGuiWindowFlags_NoNav;
#ifdef IMGUI_HAS_DOCK
            window_flags |= ImGuiWindowFlags_NoDocking;
#endif
            bool open = true;

            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::SetNextWindowPos(windowCenter, ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            if (ImGui::Begin("ProgressIndicator", &open, window_flags))
            {
                // Red color for compilation (loading case is handled by early return)
                ImVec4 const indicatorColor = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
                ui::loadingIndicatorCircle("compiling",
                                           30,
                                           indicatorColor,
                                           ImVec4(1.0f, 1.0f, 1.0f, 0.5f),
                                           12,
                                           10.0f);
                ImGui::End();
            }
            ImGui::PopStyleVar(); // WindowBorderSize
        }
    }

    void RenderWindow::updateCamera()
    {
        if (m_core)
        {
            m_core->applyCamera(m_camera);
        }
    }

    bool RenderWindow::isRenderingInProgress() const
    {
        return m_renderWindowState.isRendering;
    }

    void RenderWindow::invalidateView()
    {
        ProfileFunction;
        m_dirty = true;
        m_renderWindowState.isMoving = true;
        m_renderWindowState.currentLine = 0;
        m_renderWindowState.renderingStepSize = kInitialProgressiveStepSize;
        m_renderWindowState.isRendering = false;
        m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
        m_lastLowResRenderTime = std::chrono::system_clock::now();
        m_suppressHQDisplay.store(false, std::memory_order_release);
        
        // Skip epoch increment if preserving content during resize
        // This keeps the old frame visible while scheduling new preview
        if (!m_preserveContentDuringResize)
        {
            notifyAsyncEpochIncrement();
        }
    }

    void RenderWindow::invalidateViewDuetoModelUpdate()
    {
        // Stop streaming preview before any model/assembly mutation.
        // The streaming coroutine iterates the Assembly without a dedicated
        // lock — concurrent modification from the UI thread causes a segfault.
        stopStreamingPreview();

        // CRITICAL: Mark SDF as invalid immediately so renderer uses direct function evaluation
        // This prevents rendering with stale precomputed SDF data
        m_core->setSdfValid(false);

        // Force a low-res render on next frame for immediate visual feedback
        m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
        queueRenderDecision(m_renderUpdateCoordinator.notifyStructuralModelChanged());

        invalidateView();
        
        m_preComputedSdfDirty.store(true, std::memory_order_release);
        m_parameterDirty.store(true, std::memory_order_release);
        m_renderWindowState.renderingStepSize = kInitialProgressiveStepSize;
        m_lowResFeedbackPending.store(true, std::memory_order_release);

        // Mark model as modified for permanent centering
        m_modelModifiedSinceLastCenter = true;

        // Reset first-time bounding box availability for new model
        m_boundingBoxEverAvailable = false;

        // Reset bounding box so it will be recomputed
        m_core->resetBoundingBox();

        // DON'T schedule bbox update here - it blocks the UI thread!
        // The async SDF job scheduling in renderAsync() will handle it.
        // Just mark that we need a bbox update.
        if (m_core->isAutoUpdateBoundingBoxEnabled())
        {
            m_asyncBboxUpdatePending.store(true, std::memory_order_release);
        }
    }

    void RenderWindow::suppressHQDisplay()
    {
        m_suppressHQDisplay.store(true, std::memory_order_release);
    }

    void RenderWindow::invalidateViewDueToParameterChange()
    {
        // Streaming invalidation for parameter changes (e.g. slider drags).
        // Does NOT call invalidateView() — avoids epoch bumps that would:
        //   1. Prevent async preview results from being displayed
        //   2. Cause unnecessary churn in the render pipeline
        // The epoch is bumped later when the bbox debounce fires after drag stops,
        // starting the fresh SDF → HQ pipeline.
        m_core->setSdfValid(false);

        m_dirty = true;
        m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
        m_lastLowResRenderTime = std::chrono::system_clock::now();

        // Cancel any in-progress progressive render (stale parameters)
        m_renderWindowState.currentLine = 0;
        m_renderWindowState.renderingStepSize = kInitialProgressiveStepSize;
        m_renderWindowState.isRendering = false;

        m_preComputedSdfDirty.store(true, std::memory_order_release);
        m_parameterDirty.store(true, std::memory_order_release);
        m_lowResFeedbackPending.store(true, std::memory_order_release);
        m_modelModifiedSinceLastCenter = true;
        m_asyncViewEpoch.fetch_add(1, std::memory_order_acq_rel);
        queueRenderDecision(m_renderUpdateCoordinator.notifyParameterChanged(true));

        // Mark bbox stale instead of resetting — preserves cached bbox for reuse with extra margin
        m_core->markBoundingBoxStale();
        m_lastParameterChangeTime = std::chrono::steady_clock::now();
    }

    void RenderWindow::renderScene(RenderWindowState & state)
    {

        ProfileFunction;
        LOG_LOCATION

        if (!m_core->tryIsRenderProgramReady().value_or(false))
        {
            return;
        }
        auto computeToken = m_core->requestComputeToken();
        if (!computeToken.has_value())
        {
            return;
        }

        if (!m_enableHQRendering)
        {
            m_core->setPreCompSdfSize(128u);
        }

        if (state.isMoving)
        {
            // TODO: [003-async-preview-rendering] Consider migrating this sync path to async
            // For now, keep synchronous preview for the legacy renderSync() path
            auto const previewStatus = m_core->renderLowResPreview();
            if (previewStatus == LowResPreviewRenderStatus::Rendered)
            {
                m_lowResFeedbackPending.store(false, std::memory_order_release);
                // Track that low-res preview is now up-to-date with the current epoch
                m_lastLowResPreviewEpoch.store(m_asyncCurrentEpoch.load(std::memory_order_acquire),
                                               std::memory_order_release);
                m_lastLowResRenderTime = std::chrono::system_clock::now();
            }
            return;
        }

        if (!m_enableHQRendering)
        {
            // Clear dirty flag — with HQ disabled, low-res preview is the final output.
            m_dirty = false;
            return;
        }

        m_core->setPreCompSdfSize(256u);

        auto const timeSinceLastLowResRender =
          std::chrono::system_clock::now() - m_lastLowResRenderTime;

        if (timeSinceLastLowResRender < std::chrono::seconds(1))
        {
            return;
        }

        if (!state.isMoving)
        {
            m_core->precomputeSdfForWholeBuildPlatform();
        }
        {
            auto const maxHeight = m_core->getResultImage()->getHeight();

            if (state.currentLine < maxHeight)
            {

                if (m_core->renderScene(state.currentLine,
                                        state.currentLine + state.renderingStepSize))
                {
                    state.currentLine += state.renderingStepSize;
                }
            }
            else
            {
                m_dirty = false;
            }
        }
    }

    void RenderWindow::hide()
    {
        m_isVisible = false;
    }

    void RenderWindow::show()
    {
        m_isVisible = true;
    }

    void RenderWindow::centerView()
    {
        m_centerViewRequested = true;
        invalidateCameraView();
    }

    void RenderWindow::setTopView()
    {
        // Top view: looking down the Z axis (pitch = +90°, yaw = 0°)
        m_camera.setAngle(CL_M_PI_F / 2.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setFrontView()
    {
        // Front view: looking along the Y axis (pitch = 0°, yaw = -90°)
        m_camera.setAngle(0.0f, -CL_M_PI_F / 2.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setLeftView()
    {
        // Left view: looking along the X axis (pitch = 0°, yaw = 0°)
        m_camera.setAngle(0.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setRightView()
    {
        // Right view: looking along the negative X axis (pitch = 0°, yaw = 180°)
        m_camera.setAngle(0.0f, CL_M_PI_F);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setBackView()
    {
        // Back view: looking along the negative Y axis (pitch = 0°, yaw = 90°)
        m_camera.setAngle(0.0f, CL_M_PI_F / 2.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setBottomView()
    {
        // Bottom view: looking up the Z axis (pitch = -90°, yaw = 0°)
        m_camera.setAngle(-CL_M_PI_F / 2.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setIsometricView()
    {
        // Isometric view: pitch = -35.26°, yaw = 45° (standard CAD isometric)
        float const pitch = -std::atan(1.0f / std::sqrt(2.0f)); // ~-35.26 degrees
        float const yaw = CL_M_PI_F / 4.0f;                     // 45 degrees
        m_camera.setAngle(pitch, yaw);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::togglePerspective()
    {
        // This would require camera implementation to support orthographic/perspective toggle
        // For now, we'll save current view and restore it
        saveCurrentView();
        invalidateCameraView();
    }

    void RenderWindow::frameAll()
    {
        centerView();  // Same as center view for now
        zoomExtents(); // Zoom to fit all objects in view
    }

    void RenderWindow::zoomExtents()
    {
        // Zoom to fit all objects in view
        auto const bbox = tryFetchBoundingBox(true);
        if (!bbox.has_value())
        {
            return;
        }

        m_camera.adjustDistanceToTarget(*bbox, m_renderWindowSize_px.x, m_renderWindowSize_px.y);
        invalidateCameraView();
    }

    void RenderWindow::zoomSelected()
    {
        // For now, same as zoom extents since we don't have selection
        zoomExtents();
    }

    void RenderWindow::panLeft()
    {
        // Get current look at position and move it left
        auto currentLookAt = m_camera.getLookAt();
        Position newLookAt{currentLookAt.x - m_panSensitivity, currentLookAt.y, currentLookAt.z};
        m_camera.setLookAt(newLookAt);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panRight()
    {
        // Get current look at position and move it right
        auto currentLookAt = m_camera.getLookAt();
        Position newLookAt{currentLookAt.x + m_panSensitivity, currentLookAt.y, currentLookAt.z};
        m_camera.setLookAt(newLookAt);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panUp()
    {
        // Get current look at position and move it up
        auto currentLookAt = m_camera.getLookAt();
        Position newLookAt{currentLookAt.x, currentLookAt.y, currentLookAt.z + m_panSensitivity};
        m_camera.setLookAt(newLookAt);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panDown()
    {
        // Get current look at position and move it down
        auto currentLookAt = m_camera.getLookAt();
        Position newLookAt{currentLookAt.x, currentLookAt.y, currentLookAt.z - m_panSensitivity};
        m_camera.setLookAt(newLookAt);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::rotateLeft()
    {
        m_camera.rotate(0.0f, -m_rotateSensitivity);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::rotateRight()
    {
        m_camera.rotate(0.0f, m_rotateSensitivity);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::rotateUp()
    {
        m_camera.rotate(m_rotateSensitivity, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::rotateDown()
    {
        m_camera.rotate(-m_rotateSensitivity, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::previousView()
    {
        if (!m_viewHistory.empty() && m_currentViewIndex > 0)
        {
            m_currentViewIndex--;
            auto const & view = m_viewHistory[m_currentViewIndex];

            // Restore the view (this would require camera API support)
            // For now, we'll invalidate the view
            invalidateCameraView();
        }
    }

    void RenderWindow::nextView()
    {
        if (!m_viewHistory.empty() && m_currentViewIndex < m_viewHistory.size() - 1)
        {
            m_currentViewIndex++;
            auto const & view = m_viewHistory[m_currentViewIndex];

            // Restore the view (this would require camera API support)
            // For now, we'll invalidate the view
            invalidateCameraView();
        }
    }

    void RenderWindow::saveCurrentView()
    {
        // Save current camera state using available methods
        auto eyePos = m_camera.getEyePosition();
        auto lookAt = m_camera.getLookAt();

        m_savedView.position = Vector3{eyePos.x, eyePos.y, eyePos.z};
        m_savedView.target = Vector3{lookAt.x, lookAt.y, lookAt.z};
        m_savedView.up = Vector3{0.0f, 0.0f, 1.0f}; // Assuming Z-up
        m_savedView.distance = 100.0f;    // Default distance since we can't access private members
        m_savedView.isPerspective = true; // Default to perspective
        m_hasSavedView = true;

        // Also add to history
        CameraView currentView = m_savedView;
        m_viewHistory.push_back(currentView);
        m_currentViewIndex = m_viewHistory.size() - 1;

        // Limit history size
        if (m_viewHistory.size() > 20)
        {
            m_viewHistory.erase(m_viewHistory.begin());
            if (m_currentViewIndex > 0)
            {
                m_currentViewIndex--;
            }
        }
    }

    void RenderWindow::restoreSavedView()
    {
        if (m_hasSavedView)
        {
            // Restore the saved view (this would require camera API support)
            // For now, we'll invalidate the view
            invalidateCameraView();
        }
    }

    void RenderWindow::toggleFlyMode()
    {
        m_flyModeEnabled = !m_flyModeEnabled;
        m_cameraMode = m_flyModeEnabled ? CameraMode::Fly : CameraMode::Orbit;
        invalidateCameraView();
    }

    void RenderWindow::setOrbitMode()
    {
        m_cameraMode = CameraMode::Orbit;
        m_flyModeEnabled = false;
    }

    void RenderWindow::setPanMode()
    {
        m_cameraMode = CameraMode::Pan;
        m_flyModeEnabled = false;
    }

    void RenderWindow::setZoomMode()
    {
        m_cameraMode = CameraMode::Zoom;
        m_flyModeEnabled = false;
    }

    void RenderWindow::resetOrientation()
    {
        m_cameraMode = CameraMode::Orbit;
        m_flyModeEnabled = false;

        // Reset to isometric view
        setIsometricView();
    }

    void RenderWindow::togglePermanentCentering()
    {
        setPermanentCentering(!m_permanentCenteringEnabled);
        frameAll(); // Recenter view when toggling
    }

    void RenderWindow::setPermanentCentering(bool enabled)
    {
        m_permanentCenteringEnabled = enabled;

        // Save to config
        if (m_configManager)
        {
            m_configManager->setValue("renderWindow", "permanentCenteringEnabled", enabled);
            m_configManager->save();
        }

        if (enabled)
        {
            // Initialize tracking state
            updateCameraStateTracking();
            m_modelModifiedSinceLastCenter = true; // Force initial centering
            m_lastViewportSize = m_renderWindowSize_px;
            m_viewportSizeChangedSinceLastCenter = false;
        }
        else
        {
            // Clear tracking state when disabled
            m_lastCameraStateValid = false;
            m_viewportSizeChangedSinceLastCenter = false;
        }
    }

    bool RenderWindow::isPermanentCenteringEnabled() const
    {
        return m_permanentCenteringEnabled;
    }

    void RenderWindow::updateCameraStateTracking()
    {
        m_lastCameraState = getCurrentCameraState();
        m_lastCameraStateValid = true;
    }

    bool RenderWindow::shouldRecalculateCenter()
    {
        if (!m_permanentCenteringEnabled)
        {
            return false;
        }

        // Always recalculate if model was modified
        if (m_modelModifiedSinceLastCenter)
        {
            return true;
        }

        // Always recalculate if viewport size changed
        if (m_viewportSizeChangedSinceLastCenter)
        {
            return true;
        }

        // Check if camera has moved
        if (!m_lastCameraStateValid)
        {
            return true;
        }

        auto const currentState = getCurrentCameraState();
        return currentState != m_lastCameraState;
    }

    RenderWindow::CameraState RenderWindow::getCurrentCameraState()
    {
        CameraState state;

        // Get current camera parameters - we need to access these through the public API
        auto const eyePos = m_camera.getEyePosition();
        auto const lookAt = m_camera.getLookAt();

        state.lookAt = Position{lookAt.x, lookAt.y, lookAt.z};

        // For pitch/yaw and distance, we'd need to compute them from eye position and look at
        // Since we don't have direct access, we'll use the eye position as a proxy for changes
        Position const eyePosition{eyePos.x, eyePos.y, eyePos.z};
        Position const lookAtPosition{lookAt.x, lookAt.y, lookAt.z};
        Position const eyeToLookAt = lookAtPosition - eyePosition;

        state.distance = eyeToLookAt.norm();

        // Calculate pitch and yaw from the eye-to-lookat vector
        state.pitch = std::asin(eyeToLookAt.z() / state.distance);
        state.yaw = std::atan2(eyeToLookAt.y(), eyeToLookAt.x());

        return state;
    }

    void RenderWindow::onCameraManuallyMoved()
    {
        // When camera is manually moved, disable permanent centering temporarily
        // until the next model update or manual center request
        if (m_permanentCenteringEnabled)
        {
            updateCameraStateTracking();
        }
    }

    void RenderWindow::render(RenderWindowState & state)
    {
        ZoneScopedN("render");
        m_uiScale = ImGui::GetIO().FontGlobalScale * 2.0f;

        // During an active resize the GPU image buffers may be reallocated at
        // any moment.  Consuming async preview results that reference the old
        // (now-freed) CL images would crash in resample().  Skip all render
        // work while a deferred resize is pending — the stretched old
        // framebuffer is displayed in the meantime.
        if (m_deferredResizePending)
        {
            // Discard any pending async preview so the streaming worker doesn't
            // stall waiting for m_streamingFrameConsumed.
            if (m_asyncController && m_asyncController->tryConsumePreviewResult().has_value())
            {
                m_streamingFrameConsumed.store(true, std::memory_order_release);
            }
            return;
        }

        // Always consume async preview results, even during compilation.
        // The streaming loop may have published a frame just before compilation
        // started.  Consuming it here keeps the m_streamingFrameConsumed
        // handshake from stalling (which would make the coroutine spin on the
        // 200 ms timeout) and shows the latest pre-compilation preview.
        if (m_asyncController)
        {
            processAsyncPreviewResults();
        }

        if (!m_core->tryIsRenderProgramReady().value_or(false))
        {
            m_view->startAnimationMode();
            state.isRendering = false;
            state.renderQualityWhileMoving = 0.1f;

            // Only invalidate ONCE when compilation starts, not every frame.
            // Repeated invalidation bumps the epoch on every frame, cancelling
            // all in-flight work and preventing the pipeline from ever making
            // progress once compilation finishes.
            if (!m_compilationInvalidated)
            {
                invalidateViewDuetoModelUpdate();
                m_compilationInvalidated = true;
            }
            return;
        }

        // Post-compilation transition: re-invalidate once to set up fresh pipeline state
        // (bbox pending, SDF dirty, low-res feedback pending) now that the compiled
        // program is ready. This replaces the old pattern of invalidating every frame
        // during compilation, which spammed epoch increments.
        if (m_compilationInvalidated)
        {
            m_compilationInvalidated = false;
            // Restore preview quality — it was lowered to 0.1 during compilation.
            // The sync path's PID controller would converge it back, but the async
            // path has no PID, so we reset to the nominal 50% of full quality here.
            state.renderQualityWhileMoving = state.renderQuality * 0.5f;
            invalidateViewDuetoModelUpdate();
            queueRenderDecision(m_renderUpdateCoordinator.notifyProgramCompilationCompleted());
        }

        // Lazy initialization: only initialize async rendering once renderer is ready
        if (!m_asyncInitialized)
        {
            initializeAsyncRendering();
            m_asyncInitialized = true;
        }

        if (m_asyncConfig.wantsCoroutineBackend())
        {
            renderAsync(state);
            return;
        }

        renderSync(state);
    }

    bool RenderWindow::isAsyncBackendActive() const noexcept
    {
        return m_asyncConfig.wantsCoroutineBackend() && m_asyncInitialized && m_asyncController &&
               m_asyncController->isRunning();
    }

    std::optional<BoundingBox> RenderWindow::tryFetchBoundingBox(bool requestAsyncUpdate)
    {
        if (m_core == nullptr)
        {
            return std::nullopt;
        }

        auto bbox = m_core->getBoundingBox();
        if (bbox.has_value())
        {
            return bbox;
        }

        if (isAsyncBackendActive())
        {
            if (requestAsyncUpdate)
            {
                scheduleAsyncBboxUpdate();
            }
            return std::nullopt;
        }

        if (!m_core->updateBBox())
        {
            return std::nullopt;
        }

        return m_core->getBoundingBox();
    }

    void RenderWindow::renderSync(RenderWindowState & state)
    {
        ProfileFunction;

        // Skip all GPU rendering work while an export is in progress
        if (m_exportState != nullptr && m_exportState->isExportInProgress())
        {
            return;
        }

        // Update camera now that we know core is ready
        m_core->applyCamera(m_camera);

        m_view->stopAnimationMode();

        if (!m_dirty || state.isRendering)
        {
            return;
        }

        if (m_dirty)
        {
            m_view->startAnimationMode();
        }

        bool const firstTimeBoundingBoxAvailable =
          !m_boundingBoxEverAvailable && m_core->getBoundingBox().has_value();

        bool const shouldCenter =
          m_centerViewRequested || shouldRecalculateCenter() || firstTimeBoundingBoxAvailable;

        if (shouldCenter)
        {
            bool boundingBoxValid = m_core->getBoundingBox().has_value();
            if (boundingBoxValid)
            {
                auto bb = m_core->getBoundingBox().value();
                boundingBoxValid =
                  (fabs(bb.max.x - bb.min.x > 0.f) && fabs(bb.max.y - bb.min.y > 0.f) &&
                   fabs(bb.max.z - bb.min.z > 0.f));

                if (boundingBoxValid)
                {
                    m_camera.centerView(bb);
                    m_camera.adjustDistanceToTarget(
                      bb, m_renderWindowSize_px.x, m_renderWindowSize_px.y);

                    if (m_permanentCenteringEnabled)
                    {
                        updateCameraStateTracking();
                        m_modelModifiedSinceLastCenter = false;
                        m_viewportSizeChangedSinceLastCenter = false;
                        m_lastViewportSize = m_renderWindowSize_px;
                    }

                    m_centerViewRequested = false;

                    if (firstTimeBoundingBoxAvailable)
                    {
                        m_boundingBoxEverAvailable = true;
                    }
                }
            }

            if (!boundingBoxValid)
            {
                m_camera.setLookAt({200.0, 200.0, 50.0});

                if (m_permanentCenteringEnabled)
                {
                    updateCameraStateTracking();
                    m_modelModifiedSinceLastCenter = false;
                    m_viewportSizeChangedSinceLastCenter = false;
                    m_lastViewportSize = m_renderWindowSize_px;
                }
            }

            invalidateView();
        }
        if (state.isMoving)
        {
            if (m_preComputedSdfDirty)
            {
                m_core->getResourceContext()->getRenderingSettings().approximation = AM_FULL_MODEL;
            }
        }

        // Defer buffer reallocation during resize to preserve visible content
        bool const shouldDeferResize = m_preserveContentDuringResize && m_deferredResizePending;
        
        if (!shouldDeferResize &&
            m_core->setScreenResolution(
              static_cast<int>(
                std::clamp(m_renderWindowSize_px.x * state.renderQuality, 1.f, 16000.f)),
              static_cast<int>(
                std::clamp(m_renderWindowSize_px.y * state.renderQuality, 1.f, 16000.f))))
        {
            invalidateView();
        }

        bool previewResolutionChanged = false;

        std::pair<int, int> const lowResPreviewResolution = m_core->getLowResPreviewResolution();

        int newWidth = static_cast<int>(
          std::clamp(m_renderWindowSize_px.x * state.renderQualityWhileMoving, 1.f, 16000.f));
        int newHeight = static_cast<int>(
          std::clamp(m_renderWindowSize_px.y * state.renderQualityWhileMoving, 1.f, 16000.f));

        float widthChangePercent = std::abs(newWidth - lowResPreviewResolution.first) /
                                   static_cast<float>(lowResPreviewResolution.first) * 100.0f;
        float heightChangePercent = std::abs(newHeight - lowResPreviewResolution.second) /
                                    static_cast<float>(lowResPreviewResolution.second) * 100.0f;

        float currentAspectRatio =
          static_cast<float>(lowResPreviewResolution.first) / lowResPreviewResolution.second;
        float newAspectRatio = static_cast<float>(newWidth) / newHeight;

        if (widthChangePercent > 20.0f || heightChangePercent > 20.0f ||
            std::abs(currentAspectRatio - newAspectRatio) > 0.01f)
        {
            m_core->setLowResPreviewResolution(newWidth, newHeight);
            previewResolutionChanged = true;
        }
        state.isRendering = true;
        auto const renderFrame = [&]()
        {
            renderScene(state);

            auto const img = m_core->getResultImage();
            img->bind();
            img->unbind();
        };

        // PID controller tuning for adaptive rendering resolution
        // These values are tuned to provide smooth resolution adjustment during camera movement
        // kp: Proportional gain - primary response to frame time error
        // ki: Integral gain - eliminates steady-state error (low to prevent windup)
        // kd: Derivative gain - dampens oscillation
        float constexpr kp = 0.001f;
        float constexpr ki = 0.00001f;
        float constexpr kd = 0.000001f;

        auto const executionDuration_ms =
          measure<std::chrono::milliseconds>::execution(renderFrame);

        // Progressive rendering: cap the time the UI thread spends blocked
        // inside a single renderScene() chunk. Each call to renderScene queues
        // ONE OpenCL kernel covering [currentLine, currentLine+stepSize) and
        // waits for it to complete, so a too-large step on a heavy SDF can
        // freeze the UI for hundreds of milliseconds. The adaptive logic below
        // shrinks the step when a chunk exceeds this target and grows it back
        // when chunks are comfortably under it.
        auto constexpr progressiveTargetRenderTime_ms = 150;
        auto constexpr tolerance_ms = 1;
        auto constexpr targetFrameTime_ms = 25; // Target ~40 FPS during interaction
        float error = targetFrameTime_ms - executionDuration_ms;
        if (!previewResolutionChanged && (state.isMoving || m_core->isAnyCompilationInProgressNonBlocking()) &&
            executionDuration_ms > 0 && fabs(error) > 0)
        {
            state.fpsIntegral *= 0.8f;
            state.fpsIntegral += error;

            float derivative = error - state.fpsPreviousError;
            float adjustment = kp * error + ki * state.fpsIntegral + kd * derivative;

            state.renderQualityWhileMoving += adjustment;
            state.renderQualityWhileMoving =
              std::clamp(state.renderQualityWhileMoving, 0.05f, state.renderQuality);
        }
        if (!state.isMoving && !m_core->isAnyCompilationInProgressNonBlocking() && executionDuration_ms > 0)
        {
            if (executionDuration_ms > progressiveTargetRenderTime_ms + tolerance_ms)
            {
                auto const fraction = m_preComputedSdfDirty ? 0.1f : 0.5f;
                state.renderingStepSize =
                  std::clamp(static_cast<size_t>(state.renderingStepSize * fraction),
                             size_t{2},
                             m_core->getResultImage()->getHeight());
            }

            if (executionDuration_ms < progressiveTargetRenderTime_ms - tolerance_ms)
            {
                state.renderingStepSize =
                  std::clamp(static_cast<size_t>(state.renderingStepSize * 1.5 + 1),
                             size_t{1},
                             m_core->getResultImage()->getHeight());
            }
        }

        state.fpsPreviousError = error;
        state.isMoving = false;
        state.isRendering = false;
    }

    void RenderWindow::renderAsync(RenderWindowState & state)
    {
        ProfileFunction;

        // Skip all GPU rendering work while an export is in progress.
        // The export pipeline uses the same OpenCL context and contending for it
        // causes the UI to block on synchronous image reads.
        if (m_exportState != nullptr && m_exportState->isExportInProgress())
        {
            return;
        }

        // Update camera now that we know core is ready
        m_core->applyCamera(m_camera);
        processAsyncResults(state);

        // Process async preview results (separate from HQ progressive results)
        processAsyncPreviewResults();

                int const desiredScreenWidth = static_cast<int>(
                    std::clamp(m_renderWindowSize_px.x * state.renderQuality, 1.f, 16000.f));
                int const desiredScreenHeight = static_cast<int>(
                    std::clamp(m_renderWindowSize_px.y * state.renderQuality, 1.f, 16000.f));
                auto const resultImage = m_core->getResultImage();
                bool const screenResizeRequired =
                    resultImage && (static_cast<int>(resultImage->getWidth()) != desiredScreenWidth ||
                                                    static_cast<int>(resultImage->getHeight()) != desiredScreenHeight);

        if (m_asyncController && m_asyncController->isRunning())
        {
                        queueRenderDecision(m_renderUpdateCoordinator.configureViewport(
                            static_cast<uint32_t>(desiredScreenWidth), static_cast<uint32_t>(desiredScreenHeight)));

            m_renderUpdateCoordinator.setRealtimeGuards(
              async_rendering::RealtimeRaymarchGuards{
                .hardBlocker = !m_enableHQRendering ||
                               m_asyncSdfJobInFlight.load(std::memory_order_acquire) ||
                               m_asyncBboxJobInFlight.load(std::memory_order_acquire) ||
                               m_core->isAnyCompilationInProgressNonBlocking(),
                .renderJobInFlight = m_asyncJobInFlight.load(std::memory_order_acquire),
                .previewJobInFlight = m_asyncPreviewJobInFlight.load(std::memory_order_acquire),
                .streamingActive = m_streamingPreviewActive.load(std::memory_order_acquire),
                .streamingJobInFlight = m_streamingJobInFlight.load(std::memory_order_acquire),
                .resizePending = m_deferredResizePending || screenResizeRequired});

            if (!state.isMoving &&
                m_renderUpdateCoordinator.interactionState() ==
                  async_rendering::RenderInteractionState::CameraInteracting)
            {
                queueRenderDecision(m_renderUpdateCoordinator.notifyCameraInteractionEnded());
            }

            queueRenderDecision(m_renderUpdateCoordinator.tick());
            if (executeQueuedRenderCommands(state))
            {
                return;
            }
        }

        if (m_asyncController && m_asyncController->isRunning())
        {
            bool const sdfDirty = m_preComputedSdfDirty.load(std::memory_order_acquire);
            bool const sdfJobActive = m_asyncSdfJobInFlight.load(std::memory_order_acquire);
            bool const lowResPending = m_lowResFeedbackPending.load(std::memory_order_acquire);
            bool const bboxPending = m_asyncBboxUpdatePending.load(std::memory_order_acquire);
            bool const bboxJobActive = m_asyncBboxJobInFlight.load(std::memory_order_acquire);

            // Debounce SDF scheduling when bbox is stale (parameter drag in progress).
            // During rapid slider changes the precomputed SDF is invalidated on every
            // tick.  Computing a new SDF only to have it immediately discarded wastes
            // GPU time.  Low-res preview already works via direct function evaluation,
            // so we can safely wait until the drag stops (same debounce as bbox).
            bool const bboxStale = m_core->isBoundingBoxStale();
            bool const sdfDebounceElapsed =
              !bboxStale ||
              (std::chrono::steady_clock::now() - m_lastParameterChangeTime >= kBboxDebounceDelay);

            if (sdfDirty && !sdfJobActive && sdfDebounceElapsed)
            {
                async_rendering::RenderJob sdfJob{};
                sdfJob.type = async_rendering::RenderJobType::SDFPrecomputation;
                sdfJob.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
                if (sdfJob.epoch == 0)
                {
                    sdfJob.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
                    m_asyncCurrentEpoch.store(sdfJob.epoch, std::memory_order_release);
                }
                m_asyncController->setLatestEpoch(sdfJob.epoch);
                m_asyncSdfJobInFlight.store(true, std::memory_order_release);
                m_asyncSdfInFlightEpoch.store(sdfJob.epoch, std::memory_order_release);
                m_asyncController->enqueueJob(sdfJob);
            }

            // Schedule bbox update if pending and no bbox/SDF job is currently in flight.
            // We check sdfJobActive (captured at frame start, before new SDF scheduling)
            // rather than sdfDirty to avoid permanently blocking bbox when SDF keeps
            // failing (e.g., updateBBox() returns false because bbox was reset).
            // When SDF succeeds it computes bbox as a side effect, so bboxPending
            // is cleared in processAsyncResults to avoid redundant jobs.
            if (bboxPending && !bboxJobActive && !sdfJobActive)
            {
                scheduleAsyncBboxUpdate();
            }

            // Debounce: recompute stale bounding box after slider drag stops.
            // recomputeStaleBoundingBox() clears the cached bbox so the next SDF job
            // does a full bbox recompute with the current parameters.
            if (m_core->isBoundingBoxStale() && !bboxJobActive && !sdfJobActive)
            {
                auto const now = std::chrono::steady_clock::now();
                if (now - m_lastParameterChangeTime >= kBboxDebounceDelay)
                {
                    // Stop the streaming preview loop — drag has ended.
                    // The streaming coroutine's last iteration already pushed the
                    // final parameter values to the GPU buffer.
                    stopStreamingPreview();

                    m_core->recomputeStaleBoundingBox();
                    m_core->setSdfValid(false);
                    m_preComputedSdfDirty = true;

                    // Parameter drag has stopped — bump epoch to invalidate the stale
                    // HQ front buffer from before the drag and start fresh.
                    notifyAsyncEpochIncrement();
                    m_suppressHQDisplay.store(false, std::memory_order_release);
                }
            }

            if (lowResPending)
            {
                // Delay progressive rendering until low-res feedback is presented
                state.isRendering = false;
            }
        }

        if (!m_asyncController || !m_asyncController->isRunning())
        {
            renderSync(state);
            return;
        }

        bool const jobInFlight = m_asyncJobInFlight.load(std::memory_order_acquire);
        bool const pendingPreviewWork =
          m_forceLowResRenderOnNextFrame.load(std::memory_order_acquire) ||
          m_lowResFeedbackPending.load(std::memory_order_acquire);

        m_view->stopAnimationMode();

        if (!m_dirty && !state.isRendering && !jobInFlight && !pendingPreviewWork)
        {
            return;
        }

        m_view->startAnimationMode();

        bool const firstTimeBoundingBoxAvailable =
          !m_boundingBoxEverAvailable && m_core->getBoundingBox().has_value();

        bool const shouldCenter =
          m_centerViewRequested || shouldRecalculateCenter() || firstTimeBoundingBoxAvailable;

        if (shouldCenter)
        {
            bool boundingBoxValid = m_core->getBoundingBox().has_value();
            if (boundingBoxValid)
            {
                auto bb = m_core->getBoundingBox().value();
                boundingBoxValid =
                  (fabs(bb.max.x - bb.min.x > 0.f) && fabs(bb.max.y - bb.min.y > 0.f) &&
                   fabs(bb.max.z - bb.min.z > 0.f));

                if (boundingBoxValid)
                {
                    m_camera.centerView(bb);
                    m_camera.adjustDistanceToTarget(
                      bb, m_renderWindowSize_px.x, m_renderWindowSize_px.y);

                    if (m_permanentCenteringEnabled)
                    {
                        updateCameraStateTracking();
                        m_modelModifiedSinceLastCenter = false;
                        m_viewportSizeChangedSinceLastCenter = false;
                        m_lastViewportSize = m_renderWindowSize_px;
                    }

                    m_centerViewRequested = false;

                    if (firstTimeBoundingBoxAvailable)
                    {
                        m_boundingBoxEverAvailable = true;
                    }
                }
            }

            if (!boundingBoxValid)
            {
                m_camera.setLookAt({200.0, 200.0, 50.0});

                if (m_permanentCenteringEnabled)
                {
                    updateCameraStateTracking();
                    m_modelModifiedSinceLastCenter = false;
                    m_viewportSizeChangedSinceLastCenter = false;
                    m_lastViewportSize = m_renderWindowSize_px;
                }
            }

            // Camera-only change from centering — don't bump epoch.
            // SDF/bbox jobs are not camera-dependent and must not be cancelled.
            m_dirty = true;
            state.isMoving = true;
            state.currentLine = 0;
            state.renderingStepSize = kInitialProgressiveStepSize;
            state.isRendering = false;
            bool const forceLowResPreview =
                m_renderUpdateCoordinator.realtimeConfig().mode == async_rendering::RealtimeRaymarchMode::Off;
            m_forceLowResRenderOnNextFrame.store(forceLowResPreview, std::memory_order_release);
            if (forceLowResPreview)
            {
                m_lastLowResRenderTime = std::chrono::system_clock::now();
            }
        }

        if (state.isMoving && m_preComputedSdfDirty)
        {
            m_core->getResourceContext()->getRenderingSettings().approximation = AM_FULL_MODEL;
        }

        // Defer buffer reallocation during resize to preserve visible content. Also wait until
        // async HQ rendering has fully drained before reallocating the displayed CL/GL image:
        // worker jobs copy their staging image back into m_resultImage after the render kernel
        // completes, and resizing that image concurrently can crash inside clEnqueueCopyImage.
        bool const shouldDeferResize = m_preserveContentDuringResize && m_deferredResizePending;
        bool const hqRenderInFlight = m_asyncJobInFlight.load(std::memory_order_acquire);

        if (!shouldDeferResize && screenResizeRequired && !hqRenderInFlight &&
            m_asyncProgressiveBuffer != nullptr)
        {
            [[maybe_unused]] bool const released =
              m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                     async_rendering::FrameState::Writing,
                                                     async_rendering::FrameState::Idle);
            m_asyncProgressiveBuffer = nullptr;
            m_asyncProgressiveEpoch.store(0, std::memory_order_release);
            state.currentLine = 0;
            state.isRendering = false;
        }

        if (!shouldDeferResize && screenResizeRequired && hqRenderInFlight)
        {
            m_dirty = true;
            m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
            state.isRendering = false;
            return;
        }

        if (!shouldDeferResize &&
            m_core->setScreenResolution(desiredScreenWidth, desiredScreenHeight))
        {
            // Resolution changed - mark dirty but DON'T bump epoch if already rendering
            // (bumping epoch would cancel in-flight jobs)
            if (!state.isRendering && !m_asyncJobInFlight.load(std::memory_order_acquire))
            {
                invalidateView();
            }
            else
            {
                m_dirty = true;
            }
        }
        
        // Clear preserve flags once low-res preview starts rendering
        if (shouldDeferResize && m_forceLowResRenderOnNextFrame.load(std::memory_order_acquire))
        {
            m_preserveContentDuringResize = false;
            m_deferredResizePending = false;
        }

        std::pair<int, int> const lowResPreviewResolution = m_core->getLowResPreviewResolution();

        int newWidth = static_cast<int>(
          std::clamp(m_renderWindowSize_px.x * state.renderQualityWhileMoving, 1.f, 16000.f));
        int newHeight = static_cast<int>(
          std::clamp(m_renderWindowSize_px.y * state.renderQualityWhileMoving, 1.f, 16000.f));

        float widthChangePercent = std::abs(newWidth - lowResPreviewResolution.first) /
                                   static_cast<float>(lowResPreviewResolution.first) * 100.0f;
        float heightChangePercent = std::abs(newHeight - lowResPreviewResolution.second) /
                                    static_cast<float>(lowResPreviewResolution.second) * 100.0f;

        float currentAspectRatio =
          static_cast<float>(lowResPreviewResolution.first) / lowResPreviewResolution.second;
        float newAspectRatio = static_cast<float>(newWidth) / newHeight;

        if (widthChangePercent > 20.0f || heightChangePercent > 20.0f ||
            std::abs(currentAspectRatio - newAspectRatio) > 0.01f)
        {
            m_core->setLowResPreviewResolution(newWidth, newHeight);
        }

        // Check if we need to force a low-res render (e.g., after parameter change)
        bool const forceLowResRender =
          m_forceLowResRenderOnNextFrame.exchange(false, std::memory_order_acq_rel);
        bool const lowResPending = m_lowResFeedbackPending.load(std::memory_order_acquire);
        bool const previewJobInFlight = m_asyncPreviewJobInFlight.load(std::memory_order_acquire);

        if (m_asyncRealtimeJobInFlight.load(std::memory_order_acquire))
        {
            m_lowResFeedbackPending.store(false, std::memory_order_release);
            state.isRendering = true;
            return;
        }

        if (isRealtimeRaymarchCameraInteraction())
        {
            m_lowResFeedbackPending.store(false, std::memory_order_release);
            state.isRendering = m_asyncJobInFlight.load(std::memory_order_acquire);
            return;
        }

        if (state.isMoving || forceLowResRender || lowResPending)
        {
            bool const hadActiveProgressive =
              state.isRendering || m_asyncJobInFlight.load(std::memory_order_acquire);

            if (lowResPending && hadActiveProgressive &&
                !m_streamingPreviewActive.load(std::memory_order_acquire))
            {
                notifyAsyncEpochIncrement();
                state.currentLine = 0;
                state.renderingStepSize = kInitialProgressiveStepSize;
                state.isRendering = false;
            }

            // Decide whether to use async or sync preview:
            // - Async preview keeps the UI thread non-blocking for smooth parameter drag
            // - Sync preview is the fallback when async is unavailable
            // During parameter drag, SDF jobs are debounced so there's no GPU contention.
            bool const sdfValid = m_core->isSdfValid();
            bool const streamingActive = m_streamingPreviewActive.load(std::memory_order_acquire);
            bool const useAsyncPreview = !previewJobInFlight && sdfValid;

            if (streamingActive)
            {
                // Streaming loop handles preview rendering continuously.
                // If the job just exited (timeout/error), reschedule it here
                // rather than falling through to one-shot scheduling.
                if (!previewJobInFlight)
                {
                    scheduleStreamingPreviewJob();
                }
            }
            else if (useAsyncPreview)
            {
                // Use async preview rendering (non-blocking)
                if (scheduleAsyncPreviewJob())
                {
                    m_lastLowResRenderTime = std::chrono::system_clock::now();
                }
                else
                {
                    // Async scheduling failed - fall back to synchronous preview
                    auto const previewStatus = m_core->renderLowResPreview();
                    if (previewStatus == LowResPreviewRenderStatus::Rendered)
                    {
                        m_lowResFeedbackPending.store(false, std::memory_order_release);
                        m_lastLowResRenderTime = std::chrono::system_clock::now();
                        m_lastLowResPreviewEpoch.store(
                          m_asyncCurrentEpoch.load(std::memory_order_acquire),
                          std::memory_order_release);
                    }
                }
            }
            else if (!previewJobInFlight)
            {
                // Sync fallback when SDF job is running (GPU contention)
                auto token = m_core->requestComputeToken();
                if (token.has_value())
                {
                    auto const previewStatus = m_core->renderLowResPreview();
                    if (previewStatus == LowResPreviewRenderStatus::Rendered)
                    {
                        m_lowResFeedbackPending.store(false, std::memory_order_release);
                        m_lastLowResRenderTime = std::chrono::system_clock::now();
                        m_lastLowResPreviewEpoch.store(
                          m_asyncCurrentEpoch.load(std::memory_order_acquire),
                          std::memory_order_release);
                    }
                }
                else
                {
                    // Couldn't get compute token, retry next frame
                    m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
                }
            }
            state.isRendering = false;
            m_asyncJobInFlight.store(false, std::memory_order_release);
            return;
        }

        if (!m_enableHQRendering)
        {
            m_core->setPreCompSdfSize(128u);
            // Clear dirty flag — with HQ disabled, low-res preview is the final output.
            // Without this, m_dirty stays true forever and the UI loop never settles.
            m_dirty = false;
            return;
        }

        m_core->setPreCompSdfSize(256u);

        // Do not start HQ progressive rendering while the streaming preview loop is
        // active — both paths write to shared GPU image buffers (resultImage,
        // lowResImage) and concurrent access would cause CL errors / segfaults.
        if (m_streamingPreviewActive.load(std::memory_order_acquire) ||
            m_streamingJobInFlight.load(std::memory_order_acquire))
        {
            return;
        }

        // Only enforce timeout if we're NOT already in middle of progressive rendering
        // (otherwise chunks would be blocked after any interaction)
        bool const isProgressiveRenderInProgress = state.isRendering && state.currentLine > 0;
        bool const useLowResPreviewBeforeHq = !m_renderUpdateCoordinator.isRealtimeActive();

        if (!isProgressiveRenderInProgress && useLowResPreviewBeforeHq)
        {
            auto const timeSinceLastLowResRender =
              std::chrono::system_clock::now() - m_lastLowResRenderTime;
            if (timeSinceLastLowResRender < kBboxDebounceDelay + std::chrono::seconds(1))
            {
                return;
            }

            // Only start HQ progressive rendering if the low-res preview is up-to-date with
            // the current epoch. This prevents starting HQ rendering with stale parameters.
            auto const currentEpoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
            auto const previewEpoch = m_lastLowResPreviewEpoch.load(std::memory_order_acquire);
            if (previewEpoch < currentEpoch)
            {
                // Trigger a low-res render to update the preview epoch
                m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
                return;
            }
        }
        else {}

        if (!m_asyncJobInFlight.load(std::memory_order_acquire) &&
            !m_lowResFeedbackPending.load(std::memory_order_acquire))
        {
            state.isRendering = true;
            if (!scheduleAsyncRenderJob(state))
            {
                state.isRendering = false;
            }
            else {}
        }
        else {}
    }

    void RenderWindow::processAsyncResults(RenderWindowState & state)
    {
        ZoneScoped;
        ZoneName("ProcessAsyncResults", strlen("ProcessAsyncResults"));

        if (!m_asyncController)
        {
            return;
        }

        int resultCount = 0;
        while (auto resultOpt = m_asyncController->tryConsumeResult())
        {
            resultCount++;
            ZoneScopedN("ProcessSingleResult");
            auto & result = *resultOpt;

            auto const currentEpoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
            bool const isOutdated = result.epoch < currentEpoch;
            auto const currentViewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);
            bool const isRealtimeJob = result.jobType == async_rendering::RenderJobType::RealtimeHighQuality;
            bool const isExactRealtimeJob = isRealtimeJob;
            bool const isHqRenderJob = result.jobType == async_rendering::RenderJobType::HighQuality || isExactRealtimeJob;
            bool const isViewOutdated = isHqRenderJob && result.viewEpoch < currentViewEpoch;

            if (result.jobType == async_rendering::RenderJobType::SDFPrecomputation)
            {
                // Prevent an old SDF job from clearing the in-flight flag for a newer epoch.
                if (result.epoch == m_asyncSdfInFlightEpoch.load(std::memory_order_acquire))
                {
                    m_asyncSdfJobInFlight.store(false, std::memory_order_release);
                    m_asyncSdfInFlightEpoch.store(0, std::memory_order_release);
                }
                // Handle SDF completion: clear dirty flag and trigger preview refresh.
                // Must continue here so cancelled SDF results don't fall through to the
                // generic cancelled handler, which would clear HQ progressive render state.
                if (result.precomputedSdfUpdated && !isOutdated)
                {
                    m_preComputedSdfDirty.store(false, std::memory_order_release);
                    m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
                    // SDF computation already updated bbox (via updateBBox() in
                    // precomputeSdfAsync and commitSdfSuccess). Clear pending bbox
                    // to avoid scheduling a redundant bbox job.
                    m_asyncBboxUpdatePending.store(false, std::memory_order_release);
                }
                auto coordinatorResult = result;
                coordinatorResult.cancelled = coordinatorResult.cancelled || isOutdated;
                completeCoordinatorTask(coordinatorResult, false);
                continue;
            }

            // Handle bbox update results
            if (result.jobType == async_rendering::RenderJobType::BoundingBoxUpdate)
            {
                // Check if another update is pending
                if (m_asyncBboxUpdatePending.load(std::memory_order_acquire))
                {
                    scheduleAsyncBboxUpdate();
                }
                auto coordinatorResult = result;
                coordinatorResult.cancelled = coordinatorResult.cancelled || isOutdated;
                completeCoordinatorTask(coordinatorResult, false);
                continue;
            }

            // Skip LowResPreview results here - they are handled by processAsyncPreviewResults()
            // which consumes them via tryConsumePreviewResult() and does the resample + GL sync.
            if (result.jobType == async_rendering::RenderJobType::LowResPreview)
            {
                continue;
            }

            // Skip StreamingPreview results - they are handled via the separate preview result
            // path (setLatestPreviewResult / tryConsumePreviewResult). Without this guard the
            // result falls through to the HQ handler which incorrectly clears m_dirty and stops
            // animation mode, freezing the preview.
            if (result.jobType == async_rendering::RenderJobType::StreamingPreview)
            {
                continue;
            }

            // From here on, only HQ/full exact render results are processed.

            if (isHqRenderJob && result.computeDurationNs > 0)
            {
                size_t const renderedLines = result.completedLine > result.startLine
                                             ? result.completedLine - result.startLine
                                             : size_t{0};
                auto const durationMs = static_cast<float>(
                    static_cast<double>(result.computeDurationNs) / 1'000'000.0);
                async_rendering::RealtimeRaymarchSample sample{};
                sample.durationMs = durationMs;
                sample.width = result.width;
                sample.height = result.height;
                sample.renderedLines = renderedLines;
                sample.totalLines = result.height;
                sample.completedFrame = result.completedFrame && result.startLine == 0 &&
                                        renderedLines >= static_cast<size_t>(result.height);
                sample.cancelled = result.cancelled;
                if (isRealtimeJob)
                {
                    if (m_renderUpdateCoordinator.interactionState() ==
                        async_rendering::RenderInteractionState::Static)
                    {
                        m_renderUpdateCoordinator.recordStaticFullFrameSample(sample);
                    }
                    else
                    {
                        m_renderUpdateCoordinator.recordInteractiveRealtimeSample(sample);
                    }
                }
                else if (!isOutdated && !isViewOutdated &&
                         m_renderUpdateCoordinator.interactionState() == async_rendering::RenderInteractionState::Static)
                {
                    m_renderUpdateCoordinator.recordStaticProgressiveSample(sample);
                }
            }

            if (result.cancelled)
            {
                if (isExactRealtimeJob)
                {
                    m_renderUpdateCoordinator.recordRealtimeRejectedAttempt();
                }
                if (result.epoch == m_asyncInFlightEpoch.load(std::memory_order_acquire))
                {
                    m_asyncJobInFlight.store(false, std::memory_order_release);
                    if (isExactRealtimeJob)
                    {
                        m_asyncRealtimeJobInFlight.store(false, std::memory_order_release);
                    }
                    m_asyncInFlightEpoch.store(0, std::memory_order_release);
                    m_asyncInFlightViewEpoch.store(0, std::memory_order_release);
                    state.isRendering = false;
                }
                completeCoordinatorTask(result, false);
                continue;
            }

            // Update progressive scheduling state only for current scene/view results.
            if (!isOutdated && !isViewOutdated && result.jobType == async_rendering::RenderJobType::HighQuality)
            {
                size_t const maxHeight = static_cast<size_t>(result.height);
                state.currentLine = std::min(result.completedLine, maxHeight);
                adjustProgressFromDuration(state, result.computeDurationNs);
            }

            // Promote the newest Ready buffer to Front for display.
            // Stale scene/view results are deliberately discarded before promotion; otherwise
            // a late realtime frame could replace a valid front buffer with an old camera or
            // parameter state.
            bool const allowForcedRealtimeStaleView =
                isRealtimeJob && m_renderUpdateCoordinator.realtimeConfig().mode ==
                                   async_rendering::RealtimeRaymarchMode::Force;
            bool const discardForStaleView = isViewOutdated && !allowForcedRealtimeStaleView;

            if (result.completedFrame)
            {
                if (isOutdated || discardForStaleView)
                {
                    m_asyncController->discardReadyFrame(
                      result.frameId, result.epoch, result.viewEpoch);
                }
                else
                {
                    ZoneScopedN("PromoteReadyToFront");
                    auto * newFront = m_asyncController->promoteReadyToFront();
                    if (newFront && newFront->image)
                    {
                        // Bind the new frame to ensure GL texture is updated
                        newFront->image->invalidateContent();
                        newFront->image->bind();
                        newFront->image->unbind();

                        [[maybe_unused]] bool const promoted =
                          m_asyncController->finalizeFrontPromotion(newFront);
                    }
                }
            }

            if (result.completedFrame)
            {
                if (!isOutdated && !discardForStaleView)
                {
                    m_dirty = isViewOutdated && allowForcedRealtimeStaleView;
                    state.isRendering = false;
                    // DON'T force state.isMoving = false - let camera update control this
                    // If camera is still animating, it will set isMoving=true on next frame
                    state.currentLine = 0; // Reset for next frame
                    if (isExactRealtimeJob)
                    {
                        m_lowResFeedbackPending.store(false, std::memory_order_release);
                        m_lastLowResPreviewEpoch.store(result.epoch, std::memory_order_release);
                    }
                    if (isViewOutdated && allowForcedRealtimeStaleView)
                    {
                        m_view->startAnimationMode();
                    }
                    else
                    {
                        m_view->stopAnimationMode();
                    }
                }
            }
            else
            {
                if (isViewOutdated && m_asyncProgressiveBuffer &&
                    m_asyncProgressiveViewEpoch.load(std::memory_order_acquire) == result.viewEpoch)
                {
                    [[maybe_unused]] bool const released =
                      m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                             async_rendering::FrameState::Writing,
                                                             async_rendering::FrameState::Idle);
                    m_asyncProgressiveBuffer = nullptr;
                    m_asyncProgressiveEpoch.store(0, std::memory_order_release);
                    m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
                    state.isRendering = false;
                }
                else if (!isOutdated && !isViewOutdated && m_asyncProgressiveBuffer &&
                         m_asyncProgressiveBuffer->image)
                {
                    m_asyncProgressiveBuffer->image->invalidateContent();
                    m_view->startAnimationMode();
                }

                // Don't reset state.isRendering - we want to continue rendering
                // state.isRendering remains true to trigger next chunk scheduling
            }

            // Clear in-flight state only for the job that is actually in flight.
            if (result.epoch == m_asyncInFlightEpoch.load(std::memory_order_acquire))
            {
                m_asyncJobInFlight.store(false, std::memory_order_release);
                if (isExactRealtimeJob)
                {
                    m_asyncRealtimeJobInFlight.store(false, std::memory_order_release);
                }
                m_asyncInFlightEpoch.store(0, std::memory_order_release);
                m_asyncInFlightViewEpoch.store(0, std::memory_order_release);

                // If we just consumed an outdated job result, ensure we don't block scheduling for
                // the new epoch.
                if (isOutdated || isViewOutdated)
                {
                    state.isRendering = false;
                }
            }

            completeCoordinatorTask(result, result.completedFrame && !isOutdated && !isViewOutdated);
        }
    }

    bool RenderWindow::scheduleAsyncRenderJob(RenderWindowState & state,
                                              async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        ZoneScoped;
        ZoneName("scheduleAsyncRenderJob", strlen("scheduleAsyncRenderJob"));

        if (!m_asyncController || !m_asyncController->isRunning())
        {
            return false;
        }

        auto const image = m_core->getResultImage();
        if (!image)
        {
            return false;
        }

        size_t const height = static_cast<size_t>(image->getHeight());

        if (height == 0 || state.currentLine >= height)
        {
            m_dirty = false;
            state.isRendering = false;
            return false;
        }

        // Initialize async resources (worker queue + staging buffer) if needed
        size_t const width = static_cast<size_t>(image->getWidth());
        m_asyncController->initializeAsyncResources(*m_core->getComputeContext(), width, height);

        async_rendering::RenderJob job{};
        job.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (job.epoch == 0)
        {
            job.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_asyncCurrentEpoch.store(job.epoch, std::memory_order_release);
        }
        m_asyncController->setLatestEpoch(job.epoch);
        job.viewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);
        job.frameHint = m_asyncFrameCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
        job.type = async_rendering::RenderJobType::HighQuality;
        job.width = static_cast<uint32_t>(width);
        job.height = static_cast<uint32_t>(height);
        job.startLine = std::min(state.currentLine, height);
        if (job.startLine == 0)
        {
            state.renderingStepSize =
              std::min(state.renderingStepSize, kInitialProgressiveStepSize);
        }
        job.stepSize =
          std::max<size_t>(1, std::min(state.renderingStepSize, height - job.startLine));
        job.precomputeSdf = false;
        job.enableHighQuality = m_enableHQRendering;
        if (coordinatorTask != nullptr)
        {
            job.coordinatorRequestId = coordinatorTask->requestId;
            job.coordinatorStamp = coordinatorTask->stamp;
        }

        if (job.startLine == 0 &&
            (!m_asyncProgressiveBuffer ||
             m_asyncProgressiveEpoch.load(std::memory_order_acquire) != job.epoch))
        {
            auto * seedBuffer = m_asyncController->acquireWriteBuffer(job.epoch);
            auto renderProgram = m_core->tryGetBestRenderProgram().value_or(SharedRenderProgram{});
            bool const seedSourceFits = image->getWidth() >= width && image->getHeight() >= height;
            bool const seedTargetFits = seedBuffer && seedBuffer->image &&
                                        seedBuffer->image->getWidth() >= width &&
                                        seedBuffer->image->getHeight() >= height;
            if (seedBuffer && seedBuffer->image && renderProgram &&
                seedSourceFits && seedTargetFits)
            {
                renderProgram->resample(*image, *seedBuffer->image, 0, height);
                seedBuffer->image->invalidateContent();
                m_asyncProgressiveBuffer = seedBuffer;
                m_asyncProgressiveEpoch.store(job.epoch, std::memory_order_release);
            }
            else if (seedBuffer)
            {
                [[maybe_unused]] bool const released =
                  m_asyncController->tryTransitionBuffer(seedBuffer,
                                                         async_rendering::FrameState::Writing,
                                                         async_rendering::FrameState::Idle);
            }
        }

        ZoneValue(job.startLine);
        ZoneValue(job.stepSize);
        ZoneValue(job.epoch);

        m_asyncInFlightEpoch.store(job.epoch, std::memory_order_release);
        m_asyncJobInFlight.store(true, std::memory_order_release);
        m_asyncController->enqueueJob(job);

        ZoneText("JobEnqueued", 11);
        return true;
    }

    bool RenderWindow::scheduleRealtimeRenderJob(
      RenderWindowState & state,
      async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        ZoneScoped;
        ZoneName("scheduleRealtimeRenderJob", strlen("scheduleRealtimeRenderJob"));

        if (!m_asyncController || !m_asyncController->isRunning() || !m_enableHQRendering)
        {
            return false;
        }

        auto const image = m_core->getResultImage();
        if (!image)
        {
            return false;
        }

        size_t const width = static_cast<size_t>(image->getWidth());
        size_t const height = static_cast<size_t>(image->getHeight());
        if (width == 0 || height == 0)
        {
            return false;
        }

        m_asyncController->initializeAsyncResources(*m_core->getComputeContext(), width, height);

        if (m_asyncProgressiveBuffer && !m_asyncJobInFlight.load(std::memory_order_acquire))
        {
            [[maybe_unused]] bool const released =
              m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                     async_rendering::FrameState::Writing,
                                                     async_rendering::FrameState::Idle);
            m_asyncProgressiveBuffer = nullptr;
            m_asyncProgressiveEpoch.store(0, std::memory_order_release);
            m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
        }

        async_rendering::RenderJob job{};
        job.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (job.epoch == 0)
        {
            job.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_asyncCurrentEpoch.store(job.epoch, std::memory_order_release);
        }
        m_asyncController->setLatestEpoch(job.epoch);

        job.viewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);
        job.frameHint = m_asyncFrameCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
        job.type = async_rendering::RenderJobType::RealtimeHighQuality;
        job.width = static_cast<uint32_t>(width);
        job.height = static_cast<uint32_t>(height);
        job.startLine = 0;
        job.stepSize = height;
        job.precomputeSdf = false;
        job.enableHighQuality = true;
        if (coordinatorTask != nullptr)
        {
            job.coordinatorRequestId = coordinatorTask->requestId;
            job.coordinatorStamp = coordinatorTask->stamp;
        }

        state.currentLine = 0;
        state.renderingStepSize = job.stepSize;
        state.isRendering = true;
        m_forceLowResRenderOnNextFrame.store(false, std::memory_order_release);
        m_lowResFeedbackPending.store(false, std::memory_order_release);

        m_asyncInFlightEpoch.store(job.epoch, std::memory_order_release);
        m_asyncInFlightViewEpoch.store(job.viewEpoch, std::memory_order_release);
        m_asyncJobInFlight.store(true, std::memory_order_release);
        m_asyncRealtimeJobInFlight.store(true, std::memory_order_release);
        m_asyncController->enqueueJob(job);

        ZoneText("RealtimeJobEnqueued", 19);
        return true;
    }

    auto RenderWindow::executeAsyncRenderJob(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
        // NOTE: No ZoneScoped here — Tracy zones are not coroutine-safe across thread hops.
        // co_await waitForEvent() resumes on the OpenCL callback thread, so the zone destructor
        // would fire on a different thread than where it was created.
        // Use the inner ZoneScopedN blocks (which don't cross co_await) for profiling.
#ifdef TRACY_ENABLE
        tracy::SetThreadName("AsyncRenderWorker");
#endif

        auto * workerQueue = (m_asyncController ? m_asyncController->workerQueue() : nullptr);
        auto const & commandQueue =
          workerQueue != nullptr ? *workerQueue : m_core->getComputeContext()->GetQueue();
        if (workerQueue == nullptr) {}

        auto const cancellationRequested = [&]() -> bool { return cancelCheck && cancelCheck(); };

        async_rendering::FrameResultMeta result{};
        result.frameId = job.frameHint;
        result.epoch = job.epoch;
        result.viewEpoch = job.viewEpoch;
        result.jobType = job.type;
        result.width = job.width;
        result.height = job.height;
        result.startLine = job.startLine;
        result.coordinatorRequestId = job.coordinatorRequestId;
        result.coordinatorStamp = job.coordinatorStamp;
        bool const isExactRealtimeJob = job.type == async_rendering::RenderJobType::RealtimeHighQuality;

        if (cancellationRequested())
        {
            result.cancelled = true;
            co_return result;
        }

        if (!job.enableHighQuality)
        {
            result.cancelled = true;
            co_return result;
        }

        auto const startTime = std::chrono::steady_clock::now();

        async_rendering::FrameBuffer * writeBuffer = nullptr;

        auto releaseProgressiveBuffer = [&]()
        {
            if (!writeBuffer)
            {
                return;
            }

            [[maybe_unused]] bool const released = m_asyncController->tryTransitionBuffer(
              writeBuffer, async_rendering::FrameState::Writing, async_rendering::FrameState::Idle);

            if (writeBuffer == m_asyncProgressiveBuffer)
            {
                m_asyncProgressiveBuffer = nullptr;
                m_asyncProgressiveEpoch.store(0, std::memory_order_release);
                m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
            }

            writeBuffer = nullptr;
        };

        if (!m_asyncProgressiveBuffer ||
            m_asyncProgressiveEpoch.load(std::memory_order_acquire) != job.epoch)
        {
            writeBuffer = m_asyncController->acquireWriteBuffer(job.epoch);
            if (!writeBuffer)
            {
                result.cancelled = true;
                co_return result;
            }

            if (writeBuffer->image)
            {
                if (writeBuffer->image->getWidth() < job.width ||
                    writeBuffer->image->getHeight() < job.height)
                {
                    releaseProgressiveBuffer();
                    result.cancelled = true;
                    co_return result;
                }
            }

            m_asyncProgressiveBuffer = writeBuffer;
            m_asyncProgressiveEpoch.store(job.epoch, std::memory_order_release);
            m_asyncProgressiveViewEpoch.store(job.viewEpoch, std::memory_order_release);
        }
        else
        {
            writeBuffer = m_asyncProgressiveBuffer;
        }

        if (cancellationRequested())
        {
            releaseProgressiveBuffer();
            result.cancelled = true;
            co_return result;
        }

        try
        {
            size_t const endLine =
              std::min(job.startLine + job.stepSize, static_cast<size_t>(job.height));

            if (cancellationRequested())
            {
                result.cancelled = true;
            }
            else
            {
                if (writeBuffer != nullptr && writeBuffer->image)
                {
                    ImageRGBA & targetCLBuffer = *writeBuffer->image;

                    cl::Event renderEvent{};
                    bool advanced = false;

                    if (isExactRealtimeJob)
                    {
                        auto assembly = m_document ? m_document->getAssembly() : nullptr;
                        if (assembly && m_core->isRendererReady())
                        {
                            (void) m_core->tryToupdateParameter(*assembly);
                        }

                        auto settings = m_core->getResourceContext()->getRenderingSettings();
                        settings.approximation = AM_FULL_MODEL;
                        advanced = m_core->renderSceneComputeOnlyWithSettings(
                          commandQueue,
                          job.startLine,
                          endLine,
                          targetCLBuffer,
                          settings,
                          &renderEvent);
                    }
                    else
                    {
                        advanced = m_core->renderSceneComputeOnly(
                          commandQueue, job.startLine, endLine, targetCLBuffer, &renderEvent);
                    }

                    if (renderEvent())
                    {
                        co_await waitForEvent(renderEvent, cancelCheck);
                    }

                    if (cancellationRequested())
                    {
                        result.cancelled = true;
                    }

                    result.completedLine = advanced ? endLine : job.startLine;
                    result.completedFrame = result.completedLine >= job.height;
                    if (!advanced && isExactRealtimeJob)
                    {
                        result.cancelled = true;
                    }
                }
                else
                {
                    result.cancelled = true;
                }
            }
        }
        catch (...)
        {
            result.cancelled = true;
        }

        if (result.cancelled)
        {
            releaseProgressiveBuffer();
        }
        else if (result.completedFrame)
        {
                        m_asyncController->publishFrame(
                            writeBuffer, result.frameId, result.epoch, result.viewEpoch);
            m_asyncProgressiveBuffer = nullptr;
            m_asyncProgressiveEpoch.store(0, std::memory_order_release);
                        m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
        }

        auto const endTime = std::chrono::steady_clock::now();
        result.computeDurationNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count());

        co_return result;
    }

    void RenderWindow::adjustProgressFromDuration(RenderWindowState & state,
                                                  uint64_t computeDurationNs)
    {
        if (computeDurationNs == 0)
        {
            return;
        }

        float const executionDurationMs =
          static_cast<float>(static_cast<double>(computeDurationNs) / 1'000'000.0);
        if (executionDurationMs <= 0.0f)
        {
            return;
        }

        auto constexpr progressiveTargetRenderTime_ms = 100.0f;
        auto constexpr tolerance_ms = 1.0f;

        if (state.isMoving || m_core->isAnyCompilationInProgressNonBlocking())
        {
            return;
        }

        auto const image = m_core->getResultImage();
        if (!image)
        {
            return;
        }
        auto const maxHeight = static_cast<size_t>(image->getHeight());

        if (executionDurationMs > progressiveTargetRenderTime_ms + tolerance_ms)
        {
            auto const fraction = m_preComputedSdfDirty ? 0.1f : 0.5f;
            state.renderingStepSize = std::clamp(
              static_cast<size_t>(state.renderingStepSize * fraction), size_t{2}, maxHeight);
        }
        else if (executionDurationMs < progressiveTargetRenderTime_ms - tolerance_ms)
        {
            state.renderingStepSize = std::clamp(
              static_cast<size_t>(state.renderingStepSize * 1.5f + 1.0f), size_t{1}, maxHeight);
        }
    }

    void RenderWindow::slider(ImVec2 const & areaMin, ImVec2 const & areaMax)
    {
        ProfileFunction;

        auto & settings = m_core->getResourceContext()->getRenderingSettings();
        int renderingFlags = settings.flags;

        auto bbox = m_core->getBoundingBox();
        bool const hasBbox = bbox.has_value();

        auto z = hasBbox ? m_core->getSliceHeight() : 0.f;
        auto const maxZ = hasBbox ? bbox->max.z : 1.f;
        auto const minZ = hasBbox ? bbox->min.z : 0.f;

        // Overlay dimensions
        float constexpr sliderWidth = 20.f;
        float constexpr inputWidth = 80.f;
        float constexpr overlayWidth = std::max(sliderWidth, inputWidth);
        float constexpr padding = 6.f;
        float const areaHeight = areaMax.y - areaMin.y;
        float const inputHeight = ImGui::GetFrameHeightWithSpacing();
        float const buttonRowHeight = ImGui::GetFrameHeightWithSpacing() * 2.f;
        float const sliderHeight =
          std::max(areaHeight - inputHeight - buttonRowHeight - padding * 4.f, 30.f);
        float const overlayHeight =
          buttonRowHeight + sliderHeight + inputHeight + padding * 4.f;
        float const totalWidth = overlayWidth + padding * 2.f;

        // Position at the right edge of the render area
        ImVec2 const overlayPos = {areaMax.x - totalWidth - padding,
                                   areaMin.y + (areaHeight - overlayHeight) * 0.5f};

        // Draw semi-transparent background
        auto * drawList = ImGui::GetWindowDrawList();
        ImVec2 const bgMin = overlayPos;
        ImVec2 const bgMax = {overlayPos.x + totalWidth, overlayPos.y + overlayHeight};
        auto const & frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        ImU32 const bgColor = ImGui::ColorConvertFloat4ToU32(
          ImVec4(frameBg.x, frameBg.y, frameBg.z, 0.4f));
        drawList->AddRectFilled(bgMin, bgMax, bgColor, 8.f);

        ImGui::SetCursorScreenPos(
          ImVec2(overlayPos.x + padding, overlayPos.y + padding));

        bool zChanged = false;
        bool flagsChanged = false;

        ImGui::BeginGroup();
        {
            // Toggle buttons for Cut Off and Show Field
            bool cutOff = (renderingFlags & RF_CUT_OFF_OBJECT) != 0;
            bool showField = (renderingFlags & RF_SHOW_FIELD) != 0;

            ImVec4 const activeColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImVec4 const inactiveColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);

            // Cut-off toggle
            if (cutOff)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, inactiveColor);
            }
            if (ImGui::Button(ICON_FA_CUT "##CutToggle", ImVec2(0, 0)))
            {
                cutOff = !cutOff;
                flagsChanged = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Cut Off Object");
            }

            // Show Field toggle
            if (showField)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, inactiveColor);
            }
            if (ImGui::Button(ICON_FA_GLOBE "##FieldToggle", ImVec2(0, 0)))
            {
                showField = !showField;
                flagsChanged = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Show Distance Field");
            }

            if (flagsChanged)
            {
                if (cutOff)
                {
                    renderingFlags |= RF_CUT_OFF_OBJECT;
                }
                else
                {
                    renderingFlags &= ~RF_CUT_OFF_OBJECT;
                }
                if (showField)
                {
                    renderingFlags |= RF_SHOW_FIELD;
                }
                else
                {
                    renderingFlags &= ~RF_SHOW_FIELD;
                }
                settings.flags = renderingFlags;
                invalidateView();
            }

            // Vertical slider (thin)
            float const sliderX = overlayPos.x + padding + (overlayWidth - sliderWidth) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(sliderX, ImGui::GetCursorScreenPos().y));
            zChanged = ImGui::VSliderFloat(
              "##CutHeight", ImVec2(sliderWidth, sliderHeight), &z, minZ, maxZ, "");

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%.2f mm\nDouble-click to reset", z);
            }

            // Double-click resets
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                z = minZ;
                zChanged = true;
            }

            // Numeric input below the slider
            float const inputX = overlayPos.x + padding + (overlayWidth - inputWidth) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(inputX, ImGui::GetCursorScreenPos().y));
            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputFloat("##CutHeightInput", &z, 0.f, 0.f, "%.1f"))
            {
                z = std::clamp(z, minZ, maxZ);
                zChanged = true;
            }
        }
        ImGui::EndGroup();

        m_dirty = m_dirty || zChanged;
        m_renderWindowState.isMoving = m_renderWindowState.isMoving || zChanged;

        if (hasBbox)
        {
            m_core->setSliceHeight(z);
        }
        if (zChanged)
        {
            m_core->invalidatePreCompSdf("sliderZChanged");
            m_core->precomputeSdfForWholeBuildPlatform();
            invalidateView();
        }
    }

    void RenderWindow::renderLoadingOverlay()
    {
        ProfileFunction;

        // Show a simple loading window without requiring compute resources
        ImGuiWindowFlags const window_flags =
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, window_flags);

        // Update render window size from actual content region (bypassed the normal render path)
        m_renderWindowSize_px = {
          {ImGui::GetWindowWidth(),
           ImGui::GetWindowContentRegionMax().y - ImGui::GetWindowContentRegionMin().y}};
        float constexpr minDimension = 1.0f;
        m_renderWindowSize_px.x = std::max(m_renderWindowSize_px.x, minDimension);
        m_renderWindowSize_px.y = std::max(m_renderWindowSize_px.y, minDimension);

        // Get content area dimensions for spinner positioning
        ImVec2 const contentMin = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y};
        ImVec2 const contentMax = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y};
        ImVec2 const windowCenter = {
          0.5f * (contentMin.x + contentMax.x),
          0.5f * (contentMin.y + contentMax.y)};

        // Fill the content area without using the render texture (avoids size mismatch flicker)
        ImGui::Dummy(ImGui::GetContentRegionAvail());

        ImGui::End();
        ImGui::PopStyleVar();

        // Show loading spinner overlay
        m_view->startAnimationMode();
        ImGuiWindowFlags const overlayFlags =
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
          ImGuiWindowFlags_NoNav;
        
        bool open = true;
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(windowCenter, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        if (ImGui::Begin("ProgressIndicator", &open, overlayFlags))
        {
            // Blue color for file loading (red is used for compilation)
            ImVec4 const indicatorColor = ImVec4(0.2f, 0.6f, 1.0f, 0.8f);
            ui::loadingIndicatorCircle("loading",
                                       30,
                                       indicatorColor,
                                       ImVec4(1.0f, 1.0f, 1.0f, 0.5f),
                                       12,
                                       10.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar(); // WindowBorderSize
    }

    void RenderWindow::renderBusyOverlay()
    {
        ProfileFunction;

        // Show the last rendered frame (no clearing - keep existing image visible)
        auto displayImage = m_core->getResultImage();
        if (!displayImage)
        {
            return;
        }

        ImGuiWindowFlags const window_flags =
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, window_flags);

        // Get content area dimensions for spinner positioning
        ImVec2 const contentMin = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y};
        ImVec2 const contentMax = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y};
        ImVec2 const windowCenter = {
          0.5f * (contentMin.x + contentMax.x),
          0.5f * (contentMin.y + contentMax.y)};

        // Display the last frame (not cleared - shows previous render result)
        auto const textureId = displayImage->GetTextureId();
        ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(textureId)),
                     ImVec2(static_cast<float>(m_renderWindowSize_px.x),
                            static_cast<float>(m_renderWindowSize_px.y)));

        ImGui::End();
        ImGui::PopStyleVar();
        displayImage->unbind();

        // Show busy spinner overlay
        showProgressSpinner(windowCenter, "busy");
    }

    void RenderWindow::renderExistingFrame(std::shared_ptr<GLImageBuffer> const & displayImage)
    {
        ProfileFunction;

        // Display the existing frame without triggering new rendering
        // This keeps the UI responsive during parameter changes
        ImGuiWindowFlags const window_flags =
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, window_flags);

        // Cache window state
        m_isWindowHovered = ImGui::IsWindowHovered();
        m_isWindowFocused = ImGui::IsWindowFocused();

        // Handle key input if focused
        if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemFocused() &&
            ImGui::IsMouseHoveringRect(m_contentAreaMin, m_contentAreaMax))
        {
            handleKeyInput();
        }

        // Simple menu bar (limited functionality without compute token)
        if (ImGui::BeginMenuBar())
        {
            ImGui::EndMenuBar();
        }

        // Display the image
        auto const textureId = displayImage->GetTextureId();
        ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(textureId)),
                     ImVec2(static_cast<float>(m_renderWindowSize_px.x),
                            static_cast<float>(m_renderWindowSize_px.y)));

        // Calculate window center BEFORE calling End() (while Preview is still active)
        ImVec2 const contentMin = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y};
        ImVec2 const contentMax = {
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
          ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y};
        ImVec2 const windowCenter = {
          0.5f * (contentMin.x + contentMax.x),
          0.5f * (contentMin.y + contentMax.y)};

        ImGui::End();
        ImGui::PopStyleVar();
        displayImage->unbind();

        // Show the busy indicator only while no current preview render program is ready.
        // Background optimized compilation should not hide the command-stream preview.
        if (!m_core->tryIsRenderProgramReady().value_or(false))
        {
            showProgressSpinner(windowCenter, "compiling");
        }
    }

    void RenderWindow::showProgressSpinner(ImVec2 const & windowCenter, char const * label)
    {
        m_view->startAnimationMode();
        ImGuiWindowFlags overlayFlags =
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
          ImGuiWindowFlags_NoNav;
#ifdef IMGUI_HAS_DOCK
        overlayFlags |= ImGuiWindowFlags_NoDocking;
#endif

        bool open = true;
        ImGui::SetNextWindowBgAlpha(0.0f);

        if (ImGui::Begin("ProgressIndicator", &open, overlayFlags))
        {
            ImGui::SetWindowPos({windowCenter.x - 15.f, windowCenter.y - 15.f});
            ImVec4 const indicatorColor = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
            ui::loadingIndicatorCircle(label,
                                       30,
                                       indicatorColor,
                                       ImVec4(1.0f, 1.0f, 1.0f, 0.5f),
                                       12,
                                       10.0f);
        }
        ImGui::End();
    }

    bool RenderWindow::isVisible() const
    {
        return m_isVisible;
    }

    bool RenderWindow::isHovered() const
    {
        return m_isWindowHovered && isVisible();
    }

    bool RenderWindow::isFocused() const
    {
        return m_isWindowFocused && isVisible();
    }

    bool RenderWindow::isCameraMoving() const
    {
        return m_renderWindowState.isMoving;
    }

    void RenderWindow::handleKeyInput()
    {
        // Handle keyboard input - this can be extended later
        // For now, this is just a placeholder implementation
    }

    void RenderWindow::zoomIn()
    {
        m_camera.zoom(-0.1f); // Negative zoom means zoom in
        invalidateCameraView();
    }

    void RenderWindow::zoomOut()
    {
        m_camera.zoom(0.1f); // Positive zoom means zoom out
        invalidateCameraView();
    }

    void RenderWindow::resetZoom()
    {
        // Reset to a reasonable default distance
        // We need to access the private members, so let's use the centerView logic
        auto const bbox = tryFetchBoundingBox(true);
        if (!bbox.has_value())
        {
            return;
        }

        m_camera.adjustDistanceToTarget(*bbox, m_renderWindowSize_px.x, m_renderWindowSize_px.y);
        invalidateCameraView();
    }

    void RenderWindow::scheduleAsyncBboxUpdate(
      async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        if (!m_asyncController || !m_asyncInitialized)
        {
            return;
        }

        // If a bbox job is already in flight, mark that we need another update
        if (m_asyncBboxJobInFlight.load(std::memory_order_acquire))
        {
            m_asyncBboxUpdatePending.store(true, std::memory_order_release);
            return;
        }

        // Clear pending flag since we're starting a new job
        m_asyncBboxUpdatePending.store(false, std::memory_order_release);
        m_asyncBboxJobInFlight.store(true, std::memory_order_release);

        async_rendering::RenderJob job{};
        job.type = async_rendering::RenderJobType::BoundingBoxUpdate;
        job.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (coordinatorTask != nullptr)
        {
            job.coordinatorRequestId = coordinatorTask->requestId;
            job.coordinatorStamp = coordinatorTask->stamp;
        }

        m_asyncController->enqueueJob(job);
    }

    auto RenderWindow::executeAsyncBboxUpdate(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
        using namespace async_rendering;

        FrameResultMeta result{};
        result.epoch = job.epoch;
        result.jobType = job.type;
        result.cancelled = false;
        result.coordinatorRequestId = job.coordinatorRequestId;
        result.coordinatorStamp = job.coordinatorStamp;

        auto const startTime = std::chrono::steady_clock::now();

        // Check cancellation before starting
        if (cancelCheck())
        {
            result.cancelled = true;
            m_asyncBboxJobInFlight.store(false, std::memory_order_release);
            co_return result;
        }

        try
        {
            // Call the existing bbox computation - it's already OpenCL-based and thread-safe
            bool const success = m_core->updateBBox();
            if (!success)
            {
                result.cancelled = true;
            }
        }
        catch (std::exception const &)
        {
            result.cancelled = true;
        }
        catch (...)
        {
            result.cancelled = true;
        }

        auto const endTime = std::chrono::steady_clock::now();
        result.computeDurationNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count());

        // Clear in-flight flag
        m_asyncBboxJobInFlight.store(false, std::memory_order_release);

        // Check if another update is pending (model changed while we were computing)
        if (m_asyncBboxUpdatePending.load(std::memory_order_acquire))
        {
            // Schedule from UI thread to avoid race conditions
            // We'll do this in processAsyncResults when the result is consumed
        }

        co_return result;
    }

    auto RenderWindow::executeAsyncSdfPrecomputation(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
        // NOTE: No ZoneScoped here — Tracy zones are not coroutine-safe across thread hops.
        // co_await waitForEvent() resumes on the OpenCL callback thread.

        using namespace async_rendering;

        FrameResultMeta result{};
        result.epoch = job.epoch;
        result.jobType = job.type;
        result.cancelled = false;
        result.precomputedSdfUpdated = false;
        result.coordinatorRequestId = job.coordinatorRequestId;
        result.coordinatorStamp = job.coordinatorStamp;

        auto const startTime = std::chrono::steady_clock::now();

        // Note: We intentionally don't invalidate the old SDF when cancelled.
        // This allows preview rendering to continue with the old (slightly stale) SDF
        // during rapid parameter editing, providing a much better user experience.
        // The old SDF remains valid until a new one successfully completes.

        auto const commitSdfSuccess = [this]()
        {
            auto computeToken = m_core->waitForComputeToken();
            m_core->setSdfValid(true);
            // Skip bbox recomputation when bbox is stale (parameter drag in progress).
            // The debounced recomputeStaleBoundingBox() in renderAsync() handles it
            // once the drag stops, avoiding expensive bbox work on every parameter tick.
            if (!m_core->isBoundingBoxStale())
            {
                m_core->updateBBox();
            }
        };

        // Check cancellation before starting - just return without invalidating
        if (cancelCheck && cancelCheck())
        {
            result.cancelled = true;
            co_return result;
        }

        try
        {
            // Get worker queue (or fallback to main queue)
            auto * workerQueue = (m_asyncController ? m_asyncController->workerQueue() : nullptr);

            cl::CommandQueue const * queuePtr =
              workerQueue != nullptr ? workerQueue : &m_core->getComputeContext()->GetQueue();

            // Acquire compute token non-blockingly to serialize with refreshWorker().
            // refreshWorker() holds the mutex while it rebuilds CL programs and
            // resources — running precomputeSdfAsync concurrently would segfault.
            auto computeToken = m_core->requestComputeToken();
            if (!computeToken.has_value())
            {
                // refreshWorker (or another heavy operation) holds the lock.
                // Bail out — the next renderAsync cycle will reschedule.
                result.cancelled = true;
                co_return result;
            }

            // Launch async SDF precomputation (returns cl::Event)
            cl::Event sdfEvent = m_core->precomputeSdfAsync(*queuePtr);

            // Release the compute token — the kernel is already enqueued and the
            // CL runtime retains its own references to the memory objects.
            computeToken.reset();

            if (!sdfEvent())
            {
                // Empty event: either SDF is already valid or preconditions failed.
                // If SDF is valid, report success so sdfDirty gets cleared.
                // Otherwise treat as cancellation.
                if (m_core->isSdfValid())
                {
                    result.precomputedSdfUpdated = true;
                }
                else
                {
                    result.cancelled = true;
                }
                co_return result;
            }

            // Await SDF completion using waitForEvent helper
            co_await waitForEvent(sdfEvent, cancelCheck);

            // Check if cancelled during wait - don't invalidate, keep old SDF
            if (cancelCheck && cancelCheck())
            {
                result.cancelled = true;
                co_return result;
            }

            // SDF completed successfully
            commitSdfSuccess();
            result.precomputedSdfUpdated = true;
        }
        catch (std::exception const & e)
        {
            if (auto logger = m_core->getSharedLogger())
            {
                logger->logError(std::string("Async SDF precomputation failed: ") + e.what());
            }
            // On exception, keep old SDF valid for preview continuity
            result.cancelled = true;
        }
        catch (...)
        {
            // On exception, keep old SDF valid for preview continuity
            result.cancelled = true;
        }

        auto const endTime = std::chrono::steady_clock::now();
        result.computeDurationNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count());

        co_return result;
    }

    auto RenderWindow::executeAsyncParameterUpdate(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
        // NOTE: No ZoneScoped here — Tracy zones are not coroutine-safe across thread hops.

        using namespace async_rendering;

        FrameResultMeta result{};
        result.epoch = job.epoch;
        result.jobType = job.type;
        result.cancelled = false;

        auto const startTime = std::chrono::steady_clock::now();

        // Check cancellation before starting
        if (cancelCheck && cancelCheck())
        {
            result.cancelled = true;
            co_return result;
        }

        // Parameter updates are already very fast (<2ms) thanks to Phase 1
        // This handler exists for completeness and future double-buffering
        // For now, parameters are updated synchronously in Document::updateParameter()

        auto const endTime = std::chrono::steady_clock::now();
        result.computeDurationNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count());

        co_return result;
    }

    // ============== Async Preview Rendering ==============

    bool RenderWindow::scheduleAsyncPreviewJob(
      async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        ZoneScoped;
        ZoneName("scheduleAsyncPreviewJob", strlen("scheduleAsyncPreviewJob"));

        if (!m_asyncController || !m_asyncController->isRunning())
        {
            return false;
        }

        // Don't schedule if a preview job is already in flight
        if (m_asyncPreviewJobInFlight.load(std::memory_order_acquire))
        {
            return false;
        }

        auto const image = m_core->getResultImage();
        if (!image)
        {
            return false;
        }

        size_t const width = static_cast<size_t>(image->getWidth());
        size_t const height = static_cast<size_t>(image->getHeight());

        if (width == 0 || height == 0)
        {
            return false;
        }

        // Initialize async resources if needed
        m_asyncController->initializeAsyncResources(*m_core->getComputeContext(), width, height);

        // Create preview job
        async_rendering::RenderJob job{};
        job.type = async_rendering::RenderJobType::LowResPreview;
        job.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (job.epoch == 0)
        {
            job.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_asyncCurrentEpoch.store(job.epoch, std::memory_order_release);
        }
        m_asyncController->setLatestEpoch(job.epoch);
        job.viewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);

        // Use low-res preview resolution
        auto const [previewWidth, previewHeight] = m_core->getLowResPreviewResolution();
        job.width = static_cast<uint32_t>(previewWidth);
        job.height = static_cast<uint32_t>(previewHeight);
        job.frameHint = ++m_asyncPreviewFrameCounter;
        job.startLine = 0;
        job.stepSize = static_cast<size_t>(previewHeight);
        job.precomputeSdf = false;
        job.enableHighQuality = false;
        if (coordinatorTask != nullptr)
        {
            job.coordinatorRequestId = coordinatorTask->requestId;
            job.coordinatorStamp = coordinatorTask->stamp;
        }

        // Track preview job state
        m_asyncPreviewEpoch.store(job.epoch, std::memory_order_release);
        m_asyncPreviewJobInFlight.store(true, std::memory_order_release);
        m_asyncPreviewEnqueueTime = std::chrono::steady_clock::now();

        m_asyncController->enqueueJob(job);

        ZoneText("PreviewJobEnqueued", 18);
        return true;
    }

    auto RenderWindow::executeAsyncPreviewJob(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
        // NOTE: No ZoneScoped here — Tracy zones are not coroutine-safe across thread hops.
#ifdef TRACY_ENABLE
        tracy::SetThreadName("AsyncPreviewWorker");
#endif

        using namespace async_rendering;

        auto * workerQueue = (m_asyncController ? m_asyncController->workerQueue() : nullptr);
        auto const & commandQueue =
          workerQueue != nullptr ? *workerQueue : m_core->getComputeContext()->GetQueue();

        FrameResultMeta result{};
        result.frameId = job.frameHint;
        result.epoch = job.epoch;
        result.viewEpoch = job.viewEpoch;
        result.jobType = job.type;
        result.width = job.width;
        result.height = job.height;
        result.coordinatorRequestId = job.coordinatorRequestId;
        result.coordinatorStamp = job.coordinatorStamp;

        // Note: We do NOT cancel preview jobs based on epoch changes.
        // This ensures smooth visual feedback during camera movement.
        // We only check for shutdown requests.
        auto const shutdownRequested = [&]() -> bool {
            return cancelCheck && cancelCheck() && 
                   m_asyncController && !m_asyncController->isRunning();
        };

        auto const startTime = std::chrono::steady_clock::now();

        // Perform the async preview render using the low-res preview infrastructure.
        // Preview rendering intentionally waits for the precomputed SDF so the
        // feedback pass stays cheap and independent of mesh complexity.
        {
            ZoneScopedN("RenderLowResPreviewAsync");
            auto computeToken = m_core->waitForComputeToken();

            // Use the non-blocking async preview render
            // This renders at low resolution and returns a cl::Event for completion tracking
            auto lowResImage = m_core->getLowResPreviewImage();
            if (!lowResImage)
            {
                result.cancelled = true;
                m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                co_return result;
            }

            // Kick off the async render with distance output for HQ initialization (FR-005)
            // This populates the distance init buffer for use by subsequent HQ renders
            cl::Event renderEvent = m_core->renderLowResPreviewWithDistanceOutputAsync(commandQueue, *lowResImage);

            if (!renderEvent())
            {
                // Empty event means precondition failed (e.g., program not valid)
                result.cancelled = true;
                m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                co_return result;
            }

            // Wait for GPU completion using polling with timeout instead of blocking wait.
            // Some OpenCL implementations (e.g., rusticl) have issues with event.wait().
            // Poll with short sleeps and timeout to avoid hangs.
            auto const pollStart = std::chrono::steady_clock::now();
            auto constexpr timeout = std::chrono::milliseconds(500);
            bool eventCompleted = false;
            
            while (!eventCompleted)
            {
                try
                {
                    cl_int status = CL_QUEUED;
                    renderEvent.getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &status);
                    
                    if (status == CL_COMPLETE)
                    {
                        eventCompleted = true;
                    }
                    else if (status < 0)
                    {
                        // Error status
                        result.cancelled = true;
                        m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                        co_return result;
                    }
                    else
                    {
                        // Check timeout
                        auto const elapsed = std::chrono::steady_clock::now() - pollStart;
                        if (elapsed > timeout)
                        {
                            result.cancelled = true;
                            m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                            co_return result;
                        }
                        
                        // Sleep to avoid busy-waiting (500us reduces CPU usage while staying responsive)
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                }
                catch (std::exception const &)
                {
                    result.cancelled = true;
                    m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                    co_return result;
                }
            }

            // Only check for shutdown, not epoch-based cancellation
            if (shutdownRequested())
            {
                result.cancelled = true;
                m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
                co_return result;
            }

            // Finish the queue to ensure all work is complete before UI thread reads the buffer.
            commandQueue.finish();
            
            // Mark distance init buffer as valid - HQ renders can now use it (FR-005)
            m_core->setDistanceInitBufferValid();

            // NOTE: We intentionally do NOT resample here because resample writes to
            // m_resultImage which is a GL-interop buffer. GL operations must happen
            // on the UI thread with the GL context. The resample will be done in
            // processAsyncPreviewResults() on the UI thread.
        }

        auto const endTime = std::chrono::steady_clock::now();
        result.computeDurationNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count());
        result.completedFrame = true;

        // Store result metadata for processing
        PreviewResultMeta previewMeta{};
        previewMeta.frameId = job.frameHint;
        previewMeta.epoch = job.epoch;
        previewMeta.viewEpoch = job.viewEpoch;
        previewMeta.latencyNs = result.computeDurationNs;
        previewMeta.cancelled = false;
        previewMeta.sdfWasValid = m_core->isSdfValid();
        previewMeta.coordinatorRequestId = job.coordinatorRequestId;
        previewMeta.coordinatorStamp = job.coordinatorStamp;

        m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
        // Note: Don't set m_asyncPreviewFrameId here - it's set in processAsyncPreviewResults
        // after the frame is actually displayed on the UI thread.

        // Store the result for UI thread polling
        m_asyncController->setLatestPreviewResult(previewMeta);

        co_return result;
    }

    void RenderWindow::processAsyncPreviewResults()
    {
        ZoneScopedN("ProcessAsyncPreviewResults");

        if (!m_asyncController)
        {
            return;
        }

        // Try to consume a completed preview result
        auto result = m_asyncController->tryConsumePreviewResult();
        if (!result.has_value())
        {
            return;
        }

        auto const & meta = result.value();

                auto const currentEpoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
                auto const currentViewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);
                bool const staleForCurrentView =
                    meta.epoch < currentEpoch || (meta.viewEpoch != 0 && meta.viewEpoch < currentViewEpoch);
                bool const suppressForRealtimeRaymarch =
                    isRealtimeRaymarchCameraInteraction() ||
                    (m_renderUpdateCoordinator.isRealtimeActive() && staleForCurrentView);

        if (suppressForRealtimeRaymarch)
        {
            ZoneText("PreviewSkippedForRealtimeRaymarch", 34);
            m_streamingFrameConsumed.store(true, std::memory_order_release);
            auto cancelledMeta = meta;
            cancelledMeta.cancelled = true;
            completeCoordinatorPreviewTask(cancelledMeta);
            return;
        }

        // Skip frames that are older than what we've already displayed.
        // This ensures we never show an older frame after a newer one.
        // Use frameId (which is monotonically increasing) for ordering.
        auto const lastDisplayedFrame = m_asyncPreviewFrameId.load(std::memory_order_acquire);
        if (meta.frameId <= lastDisplayedFrame)
        {
            ZoneText("OutOfOrderPreviewSkipped", 24);
            m_streamingFrameConsumed.store(true, std::memory_order_release);
            completeCoordinatorPreviewTask(meta);
            return;
        }

        // Skip cancelled results
        if (meta.cancelled)
        {
            ZoneText("CancelledPreviewSkipped", 23);
            m_streamingFrameConsumed.store(true, std::memory_order_release);
            completeCoordinatorPreviewTask(meta);
            return;
        }

        ZoneText("PreviewResultConsumed", 21);

        // The async preview render wrote to m_lowResPreviewImage.
        // Now we need to resample to m_resultImage and sync the GL texture.
        // These operations must happen on the UI thread with the GL context.
        auto lowResImage = m_core->getLowResPreviewImage();
        auto resultImage = m_core->getResultImage();
        auto renderProgram = m_core->tryGetBestRenderProgram().value_or(SharedRenderProgram{});
        bool scheduleAdaptivePreview = false;

        if (lowResImage && resultImage && renderProgram)
        {
            // Sync GL before CL operations (matches sync renderLowResPreview pattern)
            glFinish();

            // Resample from low-res to full-res result image
            renderProgram->resample(*lowResImage, *resultImage, 0, resultImage->getHeight());

            // Mark content as dirty so bind() will transfer pixels in readpixel mode
            resultImage->invalidateContent();

            // Sync the CL buffer to the GL texture
            resultImage->bind();
            resultImage->unbind();

            // Update the last displayed frame ID for ordering
            m_asyncPreviewFrameId.store(meta.frameId, std::memory_order_release);

            if (!m_streamingPreviewActive.load(std::memory_order_acquire) &&
                !m_streamingJobInFlight.load(std::memory_order_acquire) && meta.latencyNs > 0)
            {
                float const executionDurationMs =
                  static_cast<float>(static_cast<double>(meta.latencyNs) / 1'000'000.0);
                float const error = kAdaptivePreviewTargetFrameTimeMs - executionDurationMs;

                if (executionDurationMs > 0.0f && std::abs(error) > kAdaptivePreviewMinErrorMs)
                {
                    auto & state = m_renderWindowState;
                    state.fpsIntegral *= kAdaptivePreviewIntegralDecay;
                    state.fpsIntegral += error;

                    float const derivative = error - state.fpsPreviousError;
                    float const adjustment = kAdaptivePreviewProportionalGain * error +
                                             kAdaptivePreviewIntegralGain * state.fpsIntegral +
                                             kAdaptivePreviewDerivativeGain * derivative;

                    float const previousQuality = state.renderQualityWhileMoving;
                    state.renderQualityWhileMoving =
                      std::clamp(previousQuality + adjustment,
                                 kAdaptivePreviewMinQuality,
                                 state.renderQuality);
                    state.fpsPreviousError = error;

                    int const newWidth = static_cast<int>(std::clamp(
                      m_renderWindowSize_px.x * state.renderQualityWhileMoving,
                      kAdaptivePreviewMinDimension,
                      kAdaptivePreviewMaxDimension));
                    int const newHeight = static_cast<int>(std::clamp(
                      m_renderWindowSize_px.y * state.renderQualityWhileMoving,
                      kAdaptivePreviewMinDimension,
                      kAdaptivePreviewMaxDimension));

                    auto const [currentWidth, currentHeight] =
                      m_core->getLowResPreviewResolution();
                    if (currentWidth > 0 && currentHeight > 0)
                    {
                        float const widthChangePercent =
                          std::abs(newWidth - static_cast<int>(currentWidth)) /
                          static_cast<float>(currentWidth) * 100.0f;
                        float const heightChangePercent =
                          std::abs(newHeight - static_cast<int>(currentHeight)) /
                          static_cast<float>(currentHeight) * 100.0f;

                        if (widthChangePercent > kAdaptivePreviewResizeThresholdPercent ||
                            heightChangePercent > kAdaptivePreviewResizeThresholdPercent)
                        {
                            if (m_core->setLowResPreviewResolution(
                                  static_cast<size_t>(newWidth), static_cast<size_t>(newHeight)))
                            {
                                scheduleAdaptivePreview = true;
                            }
                        }
                    }
                }
            }
        }

        // Signal the streaming worker that it is safe to render the next frame
        // into lowResImage — the resample has completed.
        m_streamingFrameConsumed.store(true, std::memory_order_release);

        // Clear low-res feedback pending flag since we now have fresh content
        m_lowResFeedbackPending.store(false, std::memory_order_release);
        m_lastLowResRenderTime = std::chrono::system_clock::now();
        m_lastLowResPreviewEpoch.store(meta.epoch, std::memory_order_release);
        completeCoordinatorPreviewTask(meta);

        if (scheduleAdaptivePreview)
        {
            m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
            m_lowResFeedbackPending.store(true, std::memory_order_release);
        }
    }

    void RenderWindow::startStreamingPreview()
    {
        if (m_core)
        {
            auto const image = m_core->getResultImage();
            if (image && image->getWidth() > 0 && image->getHeight() > 0)
            {
                auto const width = static_cast<uint32_t>(image->getWidth());
                auto const height = static_cast<uint32_t>(image->getHeight());
                queueRenderDecision(m_renderUpdateCoordinator.configureViewport(width, height));

                if (m_asyncJobInFlight.load(std::memory_order_acquire))
                {
                    return;
                }
            }
        }

        m_streamingPreviewActive.store(true, std::memory_order_release);

        // (Re-)schedule the streaming job if none is currently running.
        // This handles both first start and restart after the loop exited
        // (e.g. GPU timeout or transient error).
        if (!m_streamingJobInFlight.load(std::memory_order_acquire) &&
            !m_asyncPreviewJobInFlight.load(std::memory_order_acquire))
        {
            scheduleStreamingPreviewJob();
        }
    }

    void RenderWindow::stopStreamingPreview()
    {
        bool const wasActive = m_streamingPreviewActive.exchange(false, std::memory_order_acq_rel);
        if (wasActive)
        {
            queueRenderDecision(m_renderUpdateCoordinator.notifyParameterInteractionEnded());
        }
    }

    void RenderWindow::cancelAllAsyncWork()
    {
        stopStreamingPreview();
        notifyAsyncEpochIncrement();

        // Wait for in-flight jobs to finish before returning.
        // This is called from the UI thread before refreshWorker() starts
        // rebuilding CL programs — a running coroutine would segfault.
        auto constexpr maxWait = std::chrono::milliseconds(500);
        auto const start = std::chrono::steady_clock::now();
        while (m_streamingJobInFlight.load(std::memory_order_acquire) ||
               m_asyncSdfJobInFlight.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() - start > maxWait)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool RenderWindow::isStreamingPreviewActive() const
    {
        return m_streamingPreviewActive.load(std::memory_order_acquire);
    }

    bool RenderWindow::scheduleStreamingPreviewJob(
      async_rendering::RenderTaskRequest const * coordinatorTask)
    {
        if (!m_asyncController || !m_asyncController->isRunning())
        {
            return false;
        }

        auto lowResImage = m_core->getLowResPreviewImage();
        if (!lowResImage)
        {
            return false;
        }

        size_t const width = static_cast<size_t>(lowResImage->getWidth());
        size_t const height = static_cast<size_t>(lowResImage->getHeight());
        if (width == 0 || height == 0)
        {
            return false;
        }

        m_asyncController->initializeAsyncResources(*m_core->getComputeContext(), width, height);

        async_rendering::RenderJob job{};
        job.type = async_rendering::RenderJobType::StreamingPreview;
        job.epoch = m_asyncCurrentEpoch.load(std::memory_order_acquire);
        if (job.epoch == 0)
        {
            job.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_asyncCurrentEpoch.store(job.epoch, std::memory_order_release);
        }
        m_asyncController->setLatestEpoch(job.epoch);
        job.viewEpoch = m_asyncViewEpoch.load(std::memory_order_acquire);

        auto const [previewWidth, previewHeight] = m_core->getLowResPreviewResolution();
        job.width = static_cast<uint32_t>(previewWidth);
        job.height = static_cast<uint32_t>(previewHeight);
        job.frameHint = ++m_asyncPreviewFrameCounter;
        job.startLine = 0;
        job.stepSize = static_cast<size_t>(previewHeight);
        job.precomputeSdf = false;
        job.enableHighQuality = false;
        if (coordinatorTask != nullptr)
        {
            job.coordinatorRequestId = coordinatorTask->requestId;
            job.coordinatorStamp = coordinatorTask->stamp;
        }

        m_streamingJobInFlight.store(true, std::memory_order_release);
        m_asyncPreviewJobInFlight.store(true, std::memory_order_release);

        m_asyncController->enqueueJob(job);
        return true;
    }

    auto RenderWindow::executeStreamingPreviewJob(
      async_rendering::RenderJob const & job,
      async_rendering::AsyncRenderController::CancelCheck const & cancelCheck)
      -> coro::task<async_rendering::FrameResultMeta>
    {
#ifdef TRACY_ENABLE
        tracy::SetThreadName("StreamingPreviewWorker");
#endif
        using namespace async_rendering;

        auto * workerQueue = (m_asyncController ? m_asyncController->workerQueue() : nullptr);
        auto const & commandQueue =
          workerQueue != nullptr ? *workerQueue : m_core->getComputeContext()->GetQueue();

        FrameResultMeta result{};
        result.epoch = job.epoch;
        result.viewEpoch = job.viewEpoch;
        result.jobType = job.type;
        result.width = job.width;
        result.height = job.height;
        result.coordinatorRequestId = job.coordinatorRequestId;
        result.coordinatorStamp = job.coordinatorStamp;

        auto const shouldStop = [&]() -> bool {
            return !m_streamingPreviewActive.load(std::memory_order_acquire) ||
                   (cancelCheck && cancelCheck()) ||
                   (m_asyncController && !m_asyncController->isRunning()) ||
                   !m_core->tryIsRenderProgramReady().value_or(false);
        };

        auto lowResImage = m_core->getLowResPreviewImage();
        if (!lowResImage)
        {
            result.cancelled = true;
            m_streamingJobInFlight.store(false, std::memory_order_release);
            m_asyncPreviewJobInFlight.store(false, std::memory_order_release);
            co_return result;
        }

        // Get assembly for parameter pushing
        auto assembly = m_document ? m_document->getAssembly() : nullptr;

        uint64_t frameCounter = job.frameHint;
        int consecutiveFailures = 0;
        constexpr int kMaxConsecutiveFailures = 3;

        // Streaming loop: push params → render → publish → repeat
        while (!shouldStop())
        {
            // Wait for the UI thread to finish resampling the previous frame
            // from lowResImage before we overwrite it with the next render.
            {
                auto constexpr handshakeTimeout = std::chrono::milliseconds(200);
                auto const waitStart = std::chrono::steady_clock::now();
                while (!m_streamingFrameConsumed.load(std::memory_order_acquire))
                {
                    if (shouldStop())
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() - waitStart > handshakeTimeout)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }

            // If the UI thread has not finished resampling yet, we must NOT
            // render into lowResImage — doing so would race with resample().
            // Skip this iteration and retry; the loop condition handles exit.
            if (!m_streamingFrameConsumed.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Mark the frame as in-progress so the UI thread won't
            // resample lowResImage while we're writing to it.
            m_streamingFrameConsumed.store(false, std::memory_order_release);

            auto const iterStart = std::chrono::steady_clock::now();

            // Push latest Assembly parameter values to GPU.
            // Skip when the renderer is not ready (compilation in progress) —
            // refreshWorker modifies the Assembly without a dedicated lock,
            // so iterating it here would race with that modification.
            if (assembly && m_core->isRendererReady())
            {
                (void) m_core->tryToupdateParameter(*assembly);
            }

            // Render low-res preview
            cl::Event renderEvent =
              m_core->renderLowResPreviewWithDistanceOutputAsync(commandQueue, *lowResImage);

            if (!renderEvent())
            {
                // Precondition failed — program not valid or similar.
                // Retry a few times in case compilation is finishing.
                ++consecutiveFailures;
                if (consecutiveFailures >= kMaxConsecutiveFailures)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Poll GPU completion. Use a generous timeout for driver hiccups,
            // but preview rendering itself never falls back to full-model SDF.
            auto constexpr timeout = std::chrono::milliseconds(5000);
            auto const pollStart = std::chrono::steady_clock::now();
            bool eventCompleted = false;

            while (!eventCompleted)
            {
                try
                {
                    cl_int status = CL_QUEUED;
                    renderEvent.getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &status);

                    if (status == CL_COMPLETE)
                    {
                        eventCompleted = true;
                    }
                    else if (status < 0)
                    {
                        break; // Error
                    }
                    else
                    {
                        if (std::chrono::steady_clock::now() - pollStart > timeout)
                        {
                            break; // Timeout
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                }
                catch (std::exception const &)
                {
                    eventCompleted = false;
                    break;
                }
            }

            if (!eventCompleted)
            {
                // GPU work timed out or errored — flush (not finish!) so
                // we don't block the coroutine indefinitely on a GPU stall.
                // The next enqueue will be ordered after pending work.
                try { commandQueue.flush(); } catch (...) {}
                ++consecutiveFailures;
                if (consecutiveFailures >= kMaxConsecutiveFailures)
                {
                    break;
                }
                continue;
            }

            consecutiveFailures = 0;
            commandQueue.finish();
            m_core->setDistanceInitBufferValid();

            // Publish frame for UI thread consumption
            ++frameCounter;
            PreviewResultMeta previewMeta{};
            previewMeta.frameId = frameCounter;
            previewMeta.epoch = job.epoch;
            previewMeta.viewEpoch = job.viewEpoch;
            previewMeta.cancelled = false;
            previewMeta.sdfWasValid = m_core->isSdfValid();
            previewMeta.coordinatorRequestId = job.coordinatorRequestId;
            previewMeta.coordinatorStamp = job.coordinatorStamp;

            auto const iterEnd = std::chrono::steady_clock::now();
            previewMeta.latencyNs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(iterEnd - iterStart).count());

            m_asyncController->setLatestPreviewResult(previewMeta);
        }

        // Ensure the consumed flag is set so the UI thread or the next
        // scheduling cycle does not stay blocked.
        m_streamingFrameConsumed.store(true, std::memory_order_release);

        // Update the frame counter so future one-shot previews don't collide
        m_asyncPreviewFrameCounter = frameCounter;

        result.frameId = frameCounter;
        result.completedFrame = true;

        m_streamingJobInFlight.store(false, std::memory_order_release);
        m_asyncPreviewJobInFlight.store(false, std::memory_order_release);

        co_return result;
    }

}
