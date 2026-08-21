#include "RenderWindow.h"

#include "../ConfigManager.h"
#include "../Document.h"
#include "compute/ComputeRendererFactory.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        m_renderUpdateCoordinator.configureRealtime(loadRealtimeRaymarchConfig());
    }

    void RenderWindow::setBackendRuntime(compute::IBackendRuntime * runtime)
    {
        m_runtime = runtime;
        m_runtimeRenderBackendSession = runtime != nullptr ? runtime->getRenderBackendSession() : nullptr;
        m_renderBackendSession.reset();
        m_neutralBackendActive = runtime != nullptr &&
                                 runtime->getBackendKind() == compute::ComputeBackendKind::WebGPU;
        ++m_neutralSceneGeneration;
        m_neutralRenderScheduler.requestCancellationForAll();
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
        m_document = doc;
        m_renderBackendSession.reset();
        m_neutralViewportWidth = 0u;
        m_neutralViewportHeight = 0u;
        if (m_neutralFramePresenter)
        {
            m_neutralFramePresenter->release();
        }
        ++m_neutralSceneGeneration;
        m_neutralRenderScheduler.requestCancellationForAll();
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

        if (!session->hasMaterializedScene() ||
            session->getSceneGeneration() != m_neutralSceneGeneration)
        {
            auto const assembly = m_document->getFlatAssembly();
            if (!assembly || !assembly->assemblyModel())
            {
                return false;
            }

            auto const snapshot = compute::ComputeRendererFactory::materializeScene(
              assembly.get(), *assembly->assemblyModel(), m_neutralSceneGeneration);
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

        auto pollResult = m_neutralRenderScheduler.poll(false);
        for (auto & accepted : pollResult.acceptedFrames)
        {
            if (accepted.frame.isValid())
            {
                (void) m_neutralFramePresenter->present(accepted.frame);
            }
        }

        if (m_neutralRenderScheduler.hasInFlightSubmissions())
        {
            return true;
        }

        auto const decision = m_neutralRenderScheduler.workflow().startDisplayTask(
          async_rendering::RenderTaskType::RealtimeFullFrame);
        for (auto const & command : decision.commands)
        {
            if (command.type != async_rendering::RenderCommandType::StartTask)
            {
                continue;
            }

            if (m_neutralRenderScheduler.submit(command.task, *session))
            {
                return true;
            }

            queueRenderDecision(m_neutralRenderScheduler.workflow().completeTask(
              async_rendering::RenderTaskResult{.requestId = command.task.requestId,
                                                .type = command.task.type,
                                                .stamp = command.task.stamp,
                                                .status = async_rendering::RenderTaskStatus::Failed},
              false));
        }
        return m_neutralRenderScheduler.hasInFlightSubmissions();
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

        render(m_renderWindowState);
        auto const textureId = m_neutralFramePresenter ? m_neutralFramePresenter->getTextureId() : 0u;
        if (textureId == 0u)
        {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("Preview", &m_isVisible, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar);
        m_isWindowHovered = ImGui::IsWindowHovered();
        m_isWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_renderWindowSize_px = {ImGui::GetWindowWidth(),
                                 ImGui::GetWindowContentRegionMax().y -
                                   ImGui::GetWindowContentRegionMin().y};
        m_renderWindowSize_px.x = std::max(m_renderWindowSize_px.x, 1.0f);
        m_renderWindowSize_px.y = std::max(m_renderWindowSize_px.y, 1.0f);
        ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(textureId)),
                     {m_renderWindowSize_px.x, m_renderWindowSize_px.y});
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void RenderWindow::render(RenderWindowState & state)
    {
        if (m_neutralBackendActive)
        {
            (void) updateCameraCentering();
            (void) tryRenderWithNeutralBackend(state);
        }
    }

    void RenderWindow::updateCamera()
    {
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
        m_dirty = true;
        notifyAsyncEpochIncrement();
        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraChanged());
    }

    void RenderWindow::invalidateViewDuetoModelUpdate()
    {
        ++m_neutralSceneGeneration;
        m_renderBackendSession.reset();
        m_neutralRenderScheduler.requestCancellationForStale();
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyStructuralModelChanged());
    }

    void RenderWindow::invalidateViewDueToParameterChange()
    {
        ++m_neutralSceneGeneration;
        m_renderBackendSession.reset();
        m_neutralRenderScheduler.requestCancellationForStale();
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyParameterChanged(true));
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

    void RenderWindow::setTopView() { m_camera.setAngle(0.0f, 0.0f); invalidateCameraView(); }
    void RenderWindow::setFrontView() { m_camera.setAngle(0.0f, -1.5708f); invalidateCameraView(); }
    void RenderWindow::setLeftView() { m_camera.setAngle(0.0f, 3.14159f); invalidateCameraView(); }
    void RenderWindow::setRightView() { m_camera.setAngle(0.0f, 0.0f); invalidateCameraView(); }
    void RenderWindow::setBackView() { m_camera.setAngle(0.0f, 1.5708f); invalidateCameraView(); }
    void RenderWindow::setBottomView() { m_camera.setAngle(-1.5708f, 0.0f); invalidateCameraView(); }
    void RenderWindow::setIsometricView() { m_camera.setAngle(0.6f, -1.6f); invalidateCameraView(); }
    void RenderWindow::togglePerspective() { invalidateCameraView(); }

    void RenderWindow::zoomIn() { m_camera.zoom(-10.0f); invalidateCameraView(); }
    void RenderWindow::zoomOut() { m_camera.zoom(10.0f); invalidateCameraView(); }
    void RenderWindow::resetZoom() { m_camera.zoom(0.0f); invalidateCameraView(); }
    void RenderWindow::zoomExtents() { centerView(); }
    void RenderWindow::zoomSelected() { centerView(); }
    void RenderWindow::frameAll() { centerView(); }

    void RenderWindow::panLeft() { invalidateCameraView(); }
    void RenderWindow::panRight() { invalidateCameraView(); }
    void RenderWindow::panUp() { invalidateCameraView(); }
    void RenderWindow::panDown() { invalidateCameraView(); }
    void RenderWindow::rotateLeft() { m_camera.rotate(0.0f, -0.1f); invalidateCameraView(); }
    void RenderWindow::rotateRight() { m_camera.rotate(0.0f, 0.1f); invalidateCameraView(); }
    void RenderWindow::rotateUp() { m_camera.rotate(0.1f, 0.0f); invalidateCameraView(); }
    void RenderWindow::rotateDown() { m_camera.rotate(-0.1f, 0.0f); invalidateCameraView(); }

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
        m_renderWindowState.isMoving = true;
        m_renderWindowState.currentLine = 0u;
        m_dirty = true;
        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraChanged());
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
        for (auto const & command : decision.commands)
        {
            if (command.type == async_rendering::RenderCommandType::StartTask)
            {
                m_pendingRenderCommands.push_back(command);
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
