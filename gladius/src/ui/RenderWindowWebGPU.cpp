#include "RenderWindow.h"

#include "../ConfigManager.h"
#include "../Document.h"
#include "ShortcutManager.h"
#include "compute/ComputeRendererFactory.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace gladius::ui
{
    namespace
    {
        constexpr std::size_t kInitialProgressiveStepSize = 8u;

        [[nodiscard]] bool isRenderableBoundingBox(std::optional<BoundingBox> const & boundingBox)
        {
            if (!boundingBox.has_value())
            {
                return false;
            }

            auto const & box = *boundingBox;
            return std::isfinite(box.min.x) && std::isfinite(box.min.y) &&
                   std::isfinite(box.min.z) && std::isfinite(box.max.x) &&
                   std::isfinite(box.max.y) && std::isfinite(box.max.z) &&
                   box.max.x > box.min.x && box.max.y > box.min.y && box.max.z > box.min.z;
        }
    }

    void RenderWindow::initialize(ComputeCore * core,
                                  GLView * view,
                                  std::shared_ptr<ShortcutManager> shortcutManager,
                                  gladius::ConfigManager * configManager,
                                  compute::IBackendRuntime * runtime)
    {
        m_core = core;
        m_view = view;
        m_shortcutManager = std::move(shortcutManager);
        m_configManager = configManager;
        setBackendRuntime(runtime);
        m_neutralBackendActive = runtime != nullptr &&
                                 runtime->getBackendKind() == compute::ComputeBackendKind::WebGPU;
        if (m_configManager != nullptr)
        {
            m_permanentCenteringEnabled =
              m_configManager->getValue<bool>("renderWindow", "permanentCenteringEnabled", false);
        }
        setRenderQuality(m_neutralRenderSettings.quality);
        m_renderUpdateCoordinator.configureRealtime(loadRealtimeRaymarchConfig());
    }

    void RenderWindow::setBackendRuntime(compute::IBackendRuntime * runtime)
    {
        // A runtime replacement can disable the neutral render loop entirely. Drain before
        // releasing the old session so its physical submissions cannot remain unpolled forever.
        if (m_neutralRenderScheduler.hasInFlightSubmissions())
        {
            (void) m_neutralRenderScheduler.drain(false);
        }
        m_runtime = runtime;
        m_runtimeRenderBackendSession = runtime != nullptr ? runtime->getRenderBackendSession() : nullptr;
        m_renderBackendSession.reset();
        m_neutralBackendActive = runtime != nullptr &&
                                 runtime->getBackendKind() == compute::ComputeBackendKind::WebGPU;
    }

    compute::RenderSettingsSnapshot & RenderWindow::getNeutralRenderSettings() noexcept
    {
        return m_neutralRenderSettings;
    }

    compute::RenderSettingsSnapshot const & RenderWindow::getNeutralRenderSettings() const noexcept
    {
        return m_neutralRenderSettings;
    }

    void RenderWindow::setDocument(Document * doc)
    {
        m_neutralRenderScheduler.requestCancellationForAll();
        m_document = doc;
        m_renderBackendSession.reset();
        m_neutralViewportWidth = 0u;
        m_neutralViewportHeight = 0u;
        if (m_neutralFramePresenter)
        {
            m_neutralFramePresenter->release();
        }
        queueRenderDecision(m_renderUpdateCoordinator.notifyStructuralModelChanged());
    }

    void RenderWindow::setExportState(ExportState const * exportState)
    {
        m_exportState = exportState;
    }

    compute::RenderBackendSession * RenderWindow::getActiveRenderBackendSession() noexcept
    {
        return m_runtimeRenderBackendSession != nullptr ? m_runtimeRenderBackendSession
                                                        : m_renderBackendSession.get();
    }

    bool RenderWindow::tryRenderWithNeutralBackend(RenderWindowState & state)
    {
        if (!m_neutralBackendActive || !m_document)
        {
            return false;
        }

        if (!getActiveRenderBackendSession())
        {
            try
            {
                m_renderBackendSession = std::make_unique<compute::RenderBackendSession>(
                  compute::ComputeRendererFactory::create(compute::ComputeBackendKind::WebGPU));
            }
            catch (...)
            {
                m_renderBackendSession.reset();
            }
        }

        auto * const session = getActiveRenderBackendSession();
        if (!session)
        {
            return false;
        }

        auto const sceneGeneration = m_renderUpdateCoordinator.latestStamp().sceneEpoch;
        if (!session->hasMaterializedScene() || session->getSceneGeneration() != sceneGeneration)
        {
            auto const assembly = m_document->getFlatAssembly();
            if (!assembly || !assembly->assemblyModel())
            {
                return false;
            }

            auto const snapshot = compute::ComputeRendererFactory::materializeScene(
              assembly.get(), *assembly->assemblyModel(), sceneGeneration);
            if (!session->replaceScene(snapshot))
            {
                return false;
            }
        }

        if (!session->isAvailable() || !session->hasMaterializedScene())
        {
            return false;
        }

        if (!m_neutralFramePresenter)
        {
            m_neutralFramePresenter = std::make_unique<async_rendering::OpenGLFramePresenter>();
        }

        auto const width = static_cast<std::uint32_t>(std::clamp(
          m_renderWindowSize_px.x * state.renderQuality, 1.0f, 16000.0f));
        auto const height = static_cast<std::uint32_t>(std::clamp(
          m_renderWindowSize_px.y * state.renderQuality, 1.0f, 16000.0f));
        if (width == 0u || height == 0u)
        {
            return false;
        }

        if (m_neutralViewportWidth != width || m_neutralViewportHeight != height)
        {
            m_neutralViewportWidth = width;
            m_neutralViewportHeight = height;
            queueRenderDecision(m_neutralRenderScheduler.workflow().configureViewport(width, height));
        }

        auto pollResult = m_neutralRenderScheduler.poll(true);
        for (auto & accepted : pollResult.acceptedFrames)
        {
            if (accepted.frame.isValid())
            {
                (void) m_neutralFramePresenter->present(accepted.frame);
            }
        }
        queueRenderDecision(async_rendering::RenderWorkflowDecision{
          .commands = std::move(pollResult.commands)});

                bool const hasPendingStartTask = std::any_of(
                    m_pendingRenderCommands.begin(),
                    m_pendingRenderCommands.end(),
                    [](async_rendering::RenderCommand const & command)
                    { return command.type == async_rendering::RenderCommandType::StartTask; });
                if (!hasPendingStartTask)
        {
            queueRenderDecision(m_neutralRenderScheduler.workflow().tick());
        }

        auto pendingCommands = std::move(m_pendingRenderCommands);
        m_pendingRenderCommands.clear();
        auto retainPendingCommands = [&](std::size_t const firstCommand)
        {
            for (std::size_t index = firstCommand; index < pendingCommands.size(); ++index)
            {
                if (pendingCommands[index].type != async_rendering::RenderCommandType::StartTask)
                {
                    continue;
                }
                async_rendering::RenderWorkflowDecision pendingDecision{};
                pendingDecision.commands.push_back(std::move(pendingCommands[index]));
                queueRenderDecision(std::move(pendingDecision));
            }
        };

        for (std::size_t commandIndex = 0u; commandIndex < pendingCommands.size(); ++commandIndex)
        {
            auto const & command = pendingCommands[commandIndex];
            if (command.type != async_rendering::RenderCommandType::StartTask)
            {
                continue;
            }

            bool const isDisplayTask =
              command.task.type == async_rendering::RenderTaskType::RealtimeFullFrame ||
              command.task.type == async_rendering::RenderTaskType::StaticFullFrameProbe ||
              command.task.type == async_rendering::RenderTaskType::ProgressiveHighQualityChunk ||
              command.task.type == async_rendering::RenderTaskType::LowResolutionPreview;
            if (isDisplayTask)
            {
                // Dawn submissions remain physically owned by the backend until their terminal
                // callback. Keep at most one such submission and one newest pending command.
                if (m_neutralRenderScheduler.hasInFlightSubmissions())
                {
                    retainPendingCommands(commandIndex);
                    return true;
                }

                if (m_neutralRenderScheduler.submit(command.task, *session))
                {
                    retainPendingCommands(commandIndex + 1u);
                    return true;
                }
                queueRenderDecision(m_neutralRenderScheduler.workflow().completeTask(
                  async_rendering::RenderTaskResult{.requestId = command.task.requestId,
                                                    .type = command.task.type,
                                                    .stamp = command.task.stamp,
                                                    .status = async_rendering::RenderTaskStatus::Failed},
                  true));
            }
            else
            {
                queueRenderDecision(m_neutralRenderScheduler.workflow().completeTask(
                  async_rendering::RenderTaskResult{.requestId = command.task.requestId,
                                                    .type = command.task.type,
                                                    .stamp = command.task.stamp,
                                                    .status = async_rendering::RenderTaskStatus::Completed},
                  true));
            }
        }

        return m_neutralRenderScheduler.hasInFlightSubmissions() || !m_pendingRenderCommands.empty();
    }

    void RenderWindow::renderWindow()
    {
        if (!m_isVisible || m_view == nullptr)
        {
            return;
        }

        if (m_neutralBackendActive && !isNeutralDocumentReady())
        {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar);
        m_isWindowHovered = ImGui::IsWindowHovered();
        m_isWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_contentAreaMin = ImGui::GetWindowContentRegionMin();
        m_contentAreaMax = ImGui::GetWindowContentRegionMax();

        auto const previousSize = m_renderWindowSize_px;
        m_renderWindowSize_px = {ImGui::GetWindowWidth(),
                                 ImGui::GetWindowContentRegionMax().y -
                                   ImGui::GetWindowContentRegionMin().y};
        m_renderWindowSize_px.x = std::max(m_renderWindowSize_px.x, 1.0f);
        m_renderWindowSize_px.y = std::max(m_renderWindowSize_px.y, 1.0f);

        auto const contentMin =
          ImVec2{ImGui::GetWindowPos().x + m_contentAreaMin.x,
                 ImGui::GetWindowPos().y + m_contentAreaMin.y};
        auto const contentMax =
          ImVec2{ImGui::GetWindowPos().x + m_contentAreaMax.x,
                 ImGui::GetWindowPos().y + m_contentAreaMax.y};

        float constexpr tolerance = 1.E-4f;
        bool const sizeChanged =
          std::abs(previousSize.x - m_renderWindowSize_px.x) > tolerance ||
          std::abs(previousSize.y - m_renderWindowSize_px.y) > tolerance;
        if (sizeChanged)
        {
            m_deferredResizePending = true;
            m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
            m_lastLowResRenderTime = std::chrono::system_clock::now();
            m_dirty = true;
        }
        else if (m_deferredResizePending)
        {
            m_deferredResizePending = false;
        }

        m_cameraInputInvalidatedThisFrame = false;
        processNeutralCameraInput(contentMin, contentMax);
        render(m_renderWindowState);

        auto const textureId = m_neutralFramePresenter ? m_neutralFramePresenter->getTextureId() : 0u;
        if (textureId != 0u)
        {
            ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(textureId)),
                         {m_renderWindowSize_px.x, m_renderWindowSize_px.y});
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void RenderWindow::render(RenderWindowState & state)
    {
        if (m_neutralBackendActive)
        {
            // Keep the previously presented texture visible while the viewport dimensions
            // settle. The next frame submits with the stable size and replaces it safely.
            if (m_deferredResizePending)
            {
                return;
            }

            (void) updateCameraCentering();
            auto const cameraActuallyMoving = m_camera.update(ImGui::GetIO().DeltaTime * 1000.0f);
            if (cameraActuallyMoving)
            {
                m_cameraIdleFrames = 0;
                state.isMoving = true;
                m_lastCameraIdleTime = TimeStamp{};
                if (!m_cameraInputInvalidatedThisFrame)
                {
                    invalidateCameraView();
                }
            }
            else
            {
                ++m_cameraIdleFrames;
                state.isMoving = m_cameraIdleFrames < 10;
            }

            if (!state.isMoving &&
                m_renderUpdateCoordinator.interactionState() ==
                  async_rendering::RenderInteractionState::CameraInteracting)
            {
                auto const now = std::chrono::system_clock::now();
                if (m_lastCameraIdleTime == TimeStamp{})
                {
                    m_lastCameraIdleTime = now;
                }
                else if (now - m_lastCameraIdleTime >= std::chrono::seconds(1))
                {
                    queueRenderDecision(m_renderUpdateCoordinator.notifyCameraInteractionEnded());
                    m_lastCameraIdleTime = TimeStamp{};
                }
            }

            m_dirty = m_dirty || state.isMoving;
            (void) tryRenderWithNeutralBackend(state);

            if (m_view != nullptr)
            {
                if (state.isMoving ||
                    m_neutralRenderScheduler.hasInFlightSubmissions() ||
                    m_renderUpdateCoordinator.interactionState() ==
                      async_rendering::RenderInteractionState::CameraInteracting)
                {
                    m_view->startAnimationMode();
                }
                else
                {
                    m_view->stopAnimationMode();
                }
            }
        }
    }

    void RenderWindow::updateCamera()
    {
        (void) m_camera.update(ImGui::GetIO().DeltaTime * 1000.0f);
    }

    bool RenderWindow::isRenderingInProgress() const
    {
        return m_neutralRenderScheduler.hasInFlightSubmissions();
    }

    RenderWindow::HqProgressiveRenderProgress RenderWindow::hqProgressiveRenderProgress() const
    {
        return {};
    }

    void RenderWindow::invalidateView()
    {
        m_cameraInputInvalidatedThisFrame = true;
        m_dirty = true;
        notifyAsyncEpochIncrement();
        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraChanged(true));
        if (m_view != nullptr)
        {
            m_view->startAnimationMode();
        }
    }

    void RenderWindow::invalidateViewDuetoModelUpdate()
    {
        m_renderBackendSession.reset();
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyStructuralModelChanged());
        m_neutralRenderScheduler.requestCancellationForStale();
    }

    void RenderWindow::invalidateViewDueToParameterChange()
    {
        m_renderBackendSession.reset();
        m_dirty = true;
        queueRenderDecision(m_neutralRenderScheduler.workflow().notifyEmbeddedParameterChanged(true));
        m_neutralRenderScheduler.requestCancellationForStale();
    }

    void RenderWindow::suppressHQDisplay()
    {
    }

    void RenderWindow::startStreamingPreview()
    {
    }

    void RenderWindow::stopStreamingPreview()
    {
        m_streamingPreviewActive.store(false, std::memory_order_release);
    }

    void RenderWindow::refreshStreamingParameterInteraction()
    {
        invalidateViewDueToParameterChange();
    }

    bool RenderWindow::isStreamingPreviewActive() const
    {
        return m_streamingPreviewActive.load(std::memory_order_acquire);
    }

    async_rendering::RealtimeRaymarchMode RenderWindow::realtimeRaymarchMode() const noexcept
    {
        return m_renderUpdateCoordinator.realtimeConfig().mode;
    }

    bool RenderWindow::isRealtimeActive() const noexcept
    {
        return m_renderUpdateCoordinator.isRealtimeActive();
    }

    bool RenderWindow::isNeutralBackendActive() const noexcept
    {
        return m_neutralBackendActive;
    }

    void RenderWindow::processNeutralCameraInput(ImVec2 const & contentMin,
                                                 ImVec2 const & contentMax)
    {
        if (!m_isWindowHovered || ImGui::IsAnyItemActive())
        {
            return;
        }

        auto & io = ImGui::GetIO();
        io.MouseDragThreshold = 1.0f;
        auto const mousePos = io.MousePos;
        bool const contentHovered = ImGui::IsMouseHoveringRect(contentMin, contentMax);

        if (m_shortcutManager && contentHovered)
        {
            m_shortcutManager->processInput(ShortcutContext::RenderWindow);
        }

        if (contentHovered && m_camera.mouseMotionHandler(mousePos.x, mousePos.y))
        {
            invalidateCameraView();
        }
        if (!ImGui::IsAnyMouseDown())
        {
            m_camera.mouseInputHandler(ImGuiMouseButton_Left, -1, mousePos.x, mousePos.y);
        }

        if (!contentHovered)
        {
            return;
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_camera.mouseInputHandler(ImGuiMouseButton_Left, 0, mousePos.x, mousePos.y);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            m_camera.mouseInputHandler(ImGuiMouseButton_Right, 0, mousePos.x, mousePos.y);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            m_camera.mouseInputHandler(ImGuiMouseButton_Middle, 0, mousePos.x, mousePos.y);
        }
    }

    bool RenderWindow::isForceRealtimeMode() const noexcept
    {
        return realtimeRaymarchMode() == async_rendering::RealtimeRaymarchMode::Force;
    }

    void RenderWindow::cancelAllAsyncWork()
    {
        stopStreamingPreview();
        m_neutralRenderScheduler.requestCancellationForAll();
        notifyAsyncEpochIncrement();
    }

    void RenderWindow::renderScene(RenderWindowState & state)
    {
        render(state);
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
        invalidateView();
    }

    void RenderWindow::setTopView()
    {
        m_camera.setAngle(std::numbers::pi_v<float> / 2.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setFrontView()
    {
        m_camera.setAngle(0.0f, -std::numbers::pi_v<float> / 2.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setLeftView()
    {
        m_camera.setAngle(0.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setRightView()
    {
        m_camera.setAngle(0.0f, std::numbers::pi_v<float>);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setBackView()
    {
        m_camera.setAngle(0.0f, std::numbers::pi_v<float> / 2.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setBottomView()
    {
        m_camera.setAngle(-std::numbers::pi_v<float> / 2.0f, 0.0f);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::setIsometricView()
    {
        float const pitch = -std::atan(1.0f / std::sqrt(2.0f));
        float const yaw = std::numbers::pi_v<float> / 4.0f;
        m_camera.setAngle(pitch, yaw);
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::togglePerspective()
    {
        saveCurrentView();
        invalidateCameraView();
    }

    void RenderWindow::zoomIn()
    {
        m_camera.zoom(-0.1f);
        invalidateCameraView();
    }

    void RenderWindow::zoomOut()
    {
        m_camera.zoom(0.1f);
        invalidateCameraView();
    }

    void RenderWindow::resetZoom()
    {
        auto const boundingBox = tryFetchBoundingBox(true);
        if (!boundingBox.has_value())
        {
            return;
        }

        m_camera.adjustDistanceToTarget(*boundingBox,
                                        m_renderWindowSize_px.x,
                                        m_renderWindowSize_px.y);
        invalidateCameraView();
    }

    void RenderWindow::zoomExtents()
    {
        resetZoom();
    }

    void RenderWindow::zoomSelected()
    {
        zoomExtents();
    }

    void RenderWindow::frameAll()
    {
        centerView();
        zoomExtents();
    }

    void RenderWindow::panLeft()
    {
        auto const currentLookAt = m_camera.getLookAt();
        m_camera.setLookAt(
          Position{currentLookAt.x - m_panSensitivity, currentLookAt.y, currentLookAt.z});
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panRight()
    {
        auto const currentLookAt = m_camera.getLookAt();
        m_camera.setLookAt(
          Position{currentLookAt.x + m_panSensitivity, currentLookAt.y, currentLookAt.z});
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panUp()
    {
        auto const currentLookAt = m_camera.getLookAt();
        m_camera.setLookAt(
          Position{currentLookAt.x, currentLookAt.y, currentLookAt.z + m_panSensitivity});
        onCameraManuallyMoved();
        invalidateCameraView();
    }

    void RenderWindow::panDown()
    {
        auto const currentLookAt = m_camera.getLookAt();
        m_camera.setLookAt(
          Position{currentLookAt.x, currentLookAt.y, currentLookAt.z - m_panSensitivity});
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

    void RenderWindow::previousView() { }
    void RenderWindow::nextView() { }
    void RenderWindow::saveCurrentView() { }
    void RenderWindow::restoreSavedView() { }
    void RenderWindow::toggleFlyMode() { m_flyModeEnabled = !m_flyModeEnabled; }
    void RenderWindow::setOrbitMode() { m_cameraMode = CameraMode::Orbit; }
    void RenderWindow::setPanMode() { m_cameraMode = CameraMode::Pan; }
    void RenderWindow::setZoomMode() { m_cameraMode = CameraMode::Zoom; }
    void RenderWindow::resetOrientation() { setIsometricView(); }
    void RenderWindow::togglePermanentCentering() { setPermanentCentering(!m_permanentCenteringEnabled); }
    void RenderWindow::setPermanentCentering(bool enabled) { m_permanentCenteringEnabled = enabled; }
    bool RenderWindow::isPermanentCenteringEnabled() const { return m_permanentCenteringEnabled; }
    bool RenderWindow::isVisible() const { return m_isVisible; }
    void RenderWindow::handleKeyInput() { }
    bool RenderWindow::isHovered() const { return m_isWindowHovered; }
    bool RenderWindow::isFocused() const { return m_isWindowFocused; }
    bool RenderWindow::isCameraMoving() const { return m_renderWindowState.isMoving; }

    void RenderWindow::renderLoadingOverlay() { }
    void RenderWindow::renderBusyOverlay() { }
    void RenderWindow::renderExistingFrame(std::shared_ptr<GLImageBuffer> const &) { }
    void RenderWindow::showProgressSpinner(ImVec2 const &, char const *, ImVec4 const &) { }
    void RenderWindow::slider(ImVec2 const &, ImVec2 const &) { }
    void RenderWindow::initializeAsyncRendering() { m_asyncInitialized = true; }
    void RenderWindow::renderSync(RenderWindowState &) { }
    void RenderWindow::renderAsync(RenderWindowState &) { }
    void RenderWindow::processAsyncResults(RenderWindowState &) { }
    bool RenderWindow::scheduleAsyncRenderJob(RenderWindowState &, async_rendering::RenderTaskRequest const *) { return false; }
    bool RenderWindow::scheduleFullFrameRenderJob(RenderWindowState &, async_rendering::RenderTaskRequest const *, async_rendering::RenderJobType) { return false; }

    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeAsyncRenderJob(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }

    void RenderWindow::notifyAsyncEpochIncrement()
    {
        auto const epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1u;
        m_asyncCurrentEpoch.store(epoch, std::memory_order_release);
        m_neutralRenderScheduler.requestCancellationForStale();
    }

    void RenderWindow::invalidateCameraView()
    {
        m_cameraInputInvalidatedThisFrame = true;
        m_renderWindowState.isMoving = true;
        m_renderWindowState.currentLine = 0u;
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraChanged(true));
        m_neutralRenderScheduler.requestCancellationForStale();
        if (m_view != nullptr)
        {
            m_view->startAnimationMode();
        }
    }

    void RenderWindow::adjustProgressFromDuration(RenderWindowState &, std::uint64_t) { }

    async_rendering::RealtimeRaymarchConfig RenderWindow::loadRealtimeRaymarchConfig() const
    {
        async_rendering::RealtimeRaymarchConfig config{};
        if (m_configManager != nullptr)
        {
            config.mode = async_rendering::realtimeRaymarchModeFromString(
              m_configManager->getValue<std::string>("renderWindow", "realtimeRaymarchMode", "auto"));
            config.targetFrameTimeMs =
              m_configManager->getValue<float>("renderWindow", "realtimeRaymarchTargetMs", 25.0f);
        }
        return config;
    }

    void RenderWindow::saveRealtimeRaymarchMode(async_rendering::RealtimeRaymarchMode mode) const
    {
        if (m_configManager != nullptr)
        {
            m_configManager->setValue("renderWindow", "realtimeRaymarchMode",
                                      async_rendering::realtimeRaymarchModeToString(mode));
        }
    }

    void RenderWindow::queueRenderDecision(async_rendering::RenderWorkflowDecision decision)
    {
        auto const isInteractiveDisplayTask = [](async_rendering::RenderTaskType const type)
        {
            return type == async_rendering::RenderTaskType::RealtimeFullFrame ||
                   type == async_rendering::RenderTaskType::LowResolutionPreview ||
                   type == async_rendering::RenderTaskType::StreamingPreview;
        };

        for (auto & command : decision.commands)
        {
            if (command.type == async_rendering::RenderCommandType::StartTask)
            {
                if (isInteractiveDisplayTask(command.task.type))
                {
                    auto const oldEnd = m_pendingRenderCommands.end();
                    auto const newEnd = std::remove_if(
                      m_pendingRenderCommands.begin(),
                      oldEnd,
                      [&isInteractiveDisplayTask](async_rendering::RenderCommand const & pending)
                      {
                          return pending.type == async_rendering::RenderCommandType::StartTask &&
                                 isInteractiveDisplayTask(pending.task.type);
                      });
                    m_pendingRenderCommands.erase(newEnd, oldEnd);
                }
                m_pendingRenderCommands.push_back(std::move(command));
            }
        }
    }

    bool RenderWindow::executeQueuedRenderCommands(RenderWindowState &) { return false; }
    bool RenderWindow::executeRenderCommand(async_rendering::RenderCommand const &, RenderWindowState &) { return false; }
    bool RenderWindow::scheduleCoordinatorTask(async_rendering::RenderTaskRequest const &, RenderWindowState &) { return false; }
    bool RenderWindow::isRealtimeRaymarchInteractionActive() const noexcept { return false; }
    bool RenderWindow::scheduleAsyncSdfPrecomputation(async_rendering::RenderTaskRequest const *) { return false; }
    void RenderWindow::completeCoordinatorTask(async_rendering::RenderTaskRequest const &, async_rendering::RenderTaskStatus) { }
    void RenderWindow::completeCoordinatorTask(async_rendering::FrameResultMeta const &, bool) { }
    void RenderWindow::completeCoordinatorPreviewTask(async_rendering::PreviewResultMeta const &) { }

    bool RenderWindow::tryRenderRealtimeFrameSync(async_rendering::RenderTaskRequest const &) { return false; }

    std::optional<BoundingBox> RenderWindow::tryFetchBoundingBox(bool)
    {
        if (!m_document)
        {
            return std::nullopt;
        }
        auto const bounds = m_document->computeBoundingBox();
        return isRenderableBoundingBox(bounds) ? std::optional<BoundingBox>{bounds} : std::nullopt;
    }

    std::optional<compute::RenderBounds> RenderWindow::getNeutralModelBounds() const
    {
        if (!m_document)
        {
            return std::nullopt;
        }
        auto const bounds = m_document->computeBoundingBox();
        compute::RenderBounds result{.min = {bounds.min.x, bounds.min.y, bounds.min.z},
                                     .max = {bounds.max.x, bounds.max.y, bounds.max.z}};
        return result.isValid() ? std::optional<compute::RenderBounds>{result} : std::nullopt;
    }

    bool RenderWindow::updateCameraCentering()
    {
        if (!m_neutralBackendActive || !m_document)
        {
            return false;
        }
        auto const bounds = tryFetchBoundingBox(false);
        if (!bounds.has_value())
        {
            return false;
        }
        if (m_centerViewRequested || !m_boundingBoxEverAvailable)
        {
            m_camera.centerView(*bounds);
            m_camera.adjustDistanceToTarget(*bounds, m_renderWindowSize_px.x, m_renderWindowSize_px.y);
            m_camera.snapToTarget();
            m_centerViewRequested = false;
            m_boundingBoxEverAvailable = true;
            m_renderWindowState.isMoving = false;
            return true;
        }
        return false;
    }

    bool RenderWindow::isNeutralDocumentReady()
    {
        return m_neutralBackendActive && m_document != nullptr &&
               !m_document->isLoadingInProgress() && isRenderableBoundingBox(tryFetchBoundingBox(false));
    }

    void RenderWindow::scheduleAsyncBboxUpdate(async_rendering::RenderTaskRequest const *) { }
    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeAsyncBboxUpdate(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }
    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeAsyncSdfPrecomputation(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }
    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeAsyncParameterUpdate(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }
    bool RenderWindow::scheduleAsyncPreviewJob(async_rendering::RenderTaskRequest const *) { return false; }
    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeAsyncPreviewJob(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }
    void RenderWindow::processAsyncPreviewResults() { }
    bool RenderWindow::scheduleStreamingPreviewJob(async_rendering::RenderTaskRequest const *) { return false; }
    coro::task<async_rendering::FrameResultMeta> RenderWindow::executeStreamingPreviewJob(
      async_rendering::RenderJob const &,
      async_rendering::AsyncRenderController::CancelCheck const &)
    {
        co_return async_rendering::FrameResultMeta{};
    }
    void RenderWindow::finishParameterInteraction() { }

    void RenderWindow::updateCameraStateTracking() { }
    bool RenderWindow::shouldRecalculateCenter() { return m_centerViewRequested; }
    RenderWindow::CameraState RenderWindow::getCurrentCameraState() { return {}; }
    void RenderWindow::onCameraManuallyMoved() { }
}
