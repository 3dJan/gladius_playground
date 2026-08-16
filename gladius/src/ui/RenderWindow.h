#pragma once

#include "../types.h"
#include "GLView.h"
#include "OrbitalCamera.h"
#include "compute/ComputeCore.h"
#include "compute/OpenCLRenderRequestFactory.h"
#include "render/AsyncRenderController.h"
#include "render/NeutralRenderScheduler.h"
#include "render/OpenGLFramePresenter.h"
#include "render/RealtimeRaymarchController.h"
#include <CL/cl_platform.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace gladius::ui
{
    class ExportState;
    class ShortcutManager;
}

namespace gladius
{
    class ConfigManager;
    class Document;
}

namespace gladius::ui
{
    using TimeStamp =
      std::chrono::time_point<std::chrono::system_clock,
                              std::chrono::duration<long, std::ratio<1, 1000000000>>>;
    struct RenderWindowState
    {
        TimeStamp timeLastMove = std::chrono::system_clock::now();
        float renderQuality = 1.2f;
        float renderQualityWhileMoving = 0.02f;
        bool isRendering = false;
        bool isMoving = false;
        size_t currentLine = 0;
        size_t renderingStepSize = 100; ///< Lines rendered per frame during progressive rendering

        float fpsPreviousError = 0.0f;
        float fpsIntegral = 0.0f;
    };

    class RenderWindow
    {
      public:
        explicit RenderWindow() = default;
        void initialize(ComputeCore * core,
                        GLView * view,
                        std::shared_ptr<ShortcutManager> shortcutManager,
                        gladius::ConfigManager * configManager);

        /**
         * @brief Set the document reference for checking loading state
         * @param doc Pointer to the document (can be nullptr)
         */
        void setDocument(Document * doc);

        /// @brief Set the export state reference. Preview rendering is suppressed while
        /// an export is in progress to avoid GPU contention.
        void setExportState(ExportState const * exportState);

        /// @brief Snapshot of high-quality progressive render progress for status display.
        struct HqProgressiveRenderProgress
        {
            bool active = false;   ///< true while an HQ progressive render is advancing
            float fraction = 0.0f; ///< completion in [0, 1]
        };

        void renderWindow();
        void updateCamera();
        bool isRenderingInProgress() const;

        /// @brief Returns a snapshot of the current HQ progressive render progress.
        ///
        /// Used by the status bar to visualize progressive rendering, including when a
        /// render is aborted and restarted (the fraction jumps back toward zero).
        [[nodiscard]] HqProgressiveRenderProgress hqProgressiveRenderProgress() const;
        void invalidateView();
        void invalidateViewDuetoModelUpdate();
        void invalidateViewDueToParameterChange();

        /// Suppress HQ front-buffer display until the next full invalidation.
        /// Lightweight alternative to invalidateView() that does not bump the epoch.
        void suppressHQDisplay();

        /// Start/stop the streaming preview loop for interactive parameter editing.
        /// While active, a worker coroutine pushes the latest parameter values to the
        /// GPU and renders low-res previews in a tight loop, bypassing the per-frame
        /// scheduling round-trip through the UI thread.
        void startStreamingPreview();
        void stopStreamingPreview();
        void refreshStreamingParameterInteraction();
        [[nodiscard]] bool isStreamingPreviewActive() const;
        [[nodiscard]] async_rendering::RealtimeRaymarchMode realtimeRaymarchMode() const noexcept;
        /// Returns true when the realtime learning controller has confirmed the GPU can
        /// sustain realtime frames at the current resolution (Auto mode) or when Force mode
        /// is active.  Mirrors @ref RenderUpdateCoordinator::isRealtimeSchedulingActive().
        [[nodiscard]] bool isRealtimeActive() const noexcept;

        /// Cancel all in-flight async work (streaming preview, SDF, bbox, render jobs)
        /// by stopping streaming and bumping the epoch. Call before operations that
        /// invalidate GPU programs/resources (e.g. file load).
        void cancelAllAsyncWork();

        void renderScene(RenderWindowState & state);

        void hide();
        void show();

        void centerView();

        // Camera view methods
        void setTopView();
        void setFrontView();
        void setLeftView();
        void setRightView();
        void setBackView();
        void setBottomView();
        void setIsometricView();
        void togglePerspective();

        // Zoom methods
        void zoomIn();
        void zoomOut();
        void resetZoom();
        void zoomExtents();
        void zoomSelected();
        void frameAll();

        // Camera movement methods
        void panLeft();
        void panRight();
        void panUp();
        void panDown();
        void rotateLeft();
        void rotateRight();
        void rotateUp();
        void rotateDown();

        // View management
        void previousView();
        void nextView();
        void saveCurrentView();
        void restoreSavedView();

        // Camera modes
        void toggleFlyMode();
        void setOrbitMode();
        void setPanMode();
        void setZoomMode();
        void resetOrientation();

        // Permanent centering
        void togglePermanentCentering();
        void setPermanentCentering(bool enabled);
        [[nodiscard]] bool isPermanentCenteringEnabled() const;

        [[nodiscard]] bool isVisible() const;

        void handleKeyInput();

        /**
         * @brief Check if mouse is hovering over the render window
         * @return true if the render window is being hovered
         */
        bool isHovered() const;

        bool isFocused() const;

        /**
         * @brief Check if camera is currently moving (for UI status display)
         * @return true if the camera is being manipulated
         */
        [[nodiscard]] bool isCameraMoving() const;

        [[nodiscard]] bool isForceRealtimeMode() const noexcept;

      private:
        void render(RenderWindowState & state);
        void renderLoadingOverlay();
        void renderBusyOverlay();
        void renderExistingFrame(std::shared_ptr<GLImageBuffer> const & displayImage);
        void showProgressSpinner(ImVec2 const & windowCenter,
               char const * label,
               ImVec4 const & indicatorColor = ImVec4(1.0f, 0.0f, 0.0f, 0.8f));
        void slider(ImVec2 const & areaMin, ImVec2 const & areaMax);
        void initializeAsyncRendering();
        void renderSync(RenderWindowState & state);
        void renderAsync(RenderWindowState & state);
        void processAsyncResults(RenderWindowState & state);
        bool scheduleAsyncRenderJob(
          RenderWindowState & state,
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr);
        bool scheduleFullFrameRenderJob(
          RenderWindowState & state,
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr,
          async_rendering::RenderJobType jobType =
            async_rendering::RenderJobType::RealtimeHighQuality);
        coro::task<async_rendering::FrameResultMeta> executeAsyncRenderJob(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);
        void notifyAsyncEpochIncrement();
        void invalidateCameraView();
        void adjustProgressFromDuration(RenderWindowState & state, uint64_t computeDurationNs);
        [[nodiscard]] async_rendering::RealtimeRaymarchConfig loadRealtimeRaymarchConfig() const;
        void saveRealtimeRaymarchMode(async_rendering::RealtimeRaymarchMode mode) const;
        void queueRenderDecision(async_rendering::RenderWorkflowDecision decision);
        [[nodiscard]] bool executeQueuedRenderCommands(RenderWindowState & state);
        [[nodiscard]] bool executeRenderCommand(async_rendering::RenderCommand const & command,
                                                RenderWindowState & state);
        [[nodiscard]] bool scheduleCoordinatorTask(async_rendering::RenderTaskRequest const & task,
                                                   RenderWindowState & state);
        [[nodiscard]] bool isRealtimeRaymarchInteractionActive() const noexcept;
        [[nodiscard]] bool scheduleAsyncSdfPrecomputation(
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr);
        void completeCoordinatorTask(async_rendering::RenderTaskRequest const & request,
                                     async_rendering::RenderTaskStatus status);
        void completeCoordinatorTask(async_rendering::FrameResultMeta const & result,
                                     bool producedDisplayFrame);
        void completeCoordinatorPreviewTask(async_rendering::PreviewResultMeta const & result);

        [[nodiscard]] bool isAsyncBackendActive() const noexcept;
        [[nodiscard]] std::optional<BoundingBox> tryFetchBoundingBox(bool requestAsyncUpdate);
        [[nodiscard]] bool updateCameraCentering();

        // Async bounding box computation
        void scheduleAsyncBboxUpdate(
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr);
        coro::task<async_rendering::FrameResultMeta> executeAsyncBboxUpdate(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);

        // Async SDF precomputation
        coro::task<async_rendering::FrameResultMeta> executeAsyncSdfPrecomputation(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);

        // Async parameter update
        coro::task<async_rendering::FrameResultMeta> executeAsyncParameterUpdate(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);

        // Async preview rendering (non-blocking low-res preview during camera movement)
        bool scheduleAsyncPreviewJob(
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr);
        coro::task<async_rendering::FrameResultMeta> executeAsyncPreviewJob(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);
        void processAsyncPreviewResults();

        // Streaming preview loop (tight render loop during parameter drag)
        bool scheduleStreamingPreviewJob(
          async_rendering::RenderTaskRequest const * coordinatorTask = nullptr);
        coro::task<async_rendering::FrameResultMeta> executeStreamingPreviewJob(
          async_rendering::RenderJob const & job,
          async_rendering::AsyncRenderController::CancelCheck const & cancelCheck);
        void finishParameterInteraction();

        /// Render a single full-quality realtime frame synchronously on the UI thread.
        /// Pushes the latest parameters to the GPU, submits the OpenCL render kernel,
        /// blocks until the GPU is done, then immediately promotes the result to the
        /// front buffer for display.  Returns true on success.
        ///
        /// Used when @ref RenderUpdateCoordinator::isRealtimeSchedulingActive() is true so
        /// that both Force mode and Auto mode (once performance is proven) get direct,
        /// zero-latency parameter feedback without going through the async job queue.
        /// SDF precomputation and bounding-box updates remain asynchronous.
        [[nodiscard]] bool tryRenderRealtimeFrameSync(
          async_rendering::RenderTaskRequest const & task);
        [[nodiscard]] bool tryRenderWithNeutralBackend(RenderWindowState & state);

        GLView * m_view{};

        ComputeCore * m_core;
        Document * m_document{nullptr};
        ExportState const * m_exportState{nullptr};
        std::shared_ptr<ShortcutManager> m_shortcutManager;
        gladius::ConfigManager * m_configManager;

        std::atomic<bool> m_dirty{true};
        std::atomic<bool> m_parameterDirty{false};
        std::atomic<bool> m_suppressHQDisplay{false};
        std::atomic<bool> m_preComputedSdfDirty{true};
        std::atomic<bool> m_forceLowResRenderOnNextFrame{false};

        ui::OrbitalCamera m_camera;

        float2 m_renderWindowSize_px{128, 128};

        bool m_isVisible{true};

        // Cached ImGui window state (updated during rendering)
        mutable bool m_isWindowHovered{false};
        mutable bool m_isWindowFocused{false};

        RenderWindowState m_renderWindowState{};

        bool m_centerViewRequested = false;
        int m_cameraIdleFrames{0};
        bool m_enableHQRendering = true;

        ImVec2 m_contentAreaMin;
        ImVec2 m_contentAreaMax;

        TimeStamp m_lastLowResRenderTime;
        TimeStamp m_lastCameraIdleTime; ///< When camera last became idle (for interaction end debounce)

        float m_uiScale = 1.0f;

        // View management
        struct CameraView
        {
            Vector3 position;
            Vector3 target;
            Vector3 up;
            float distance;
            bool isPerspective;
        };

        std::vector<CameraView> m_viewHistory;
        size_t m_currentViewIndex = 0;
        CameraView m_savedView;
        bool m_hasSavedView = false;

        // Camera modes
        enum class CameraMode
        {
            Orbit,
            Pan,
            Zoom,
            Fly
        };

        CameraMode m_cameraMode = CameraMode::Orbit;
        bool m_flyModeEnabled = false;

        // Camera movement parameters
        float m_panSensitivity = 0.1f;
        float m_rotateSensitivity = 0.02f;
        float m_zoomSensitivity = 0.1f;

        // Permanent centering state
        bool m_permanentCenteringEnabled = false;
        bool m_lastCameraStateValid = false;

        // First-time centering for new models
        bool m_boundingBoxEverAvailable = false;

        // Camera state tracking for permanent centering
        struct CameraState
        {
            Position lookAt;
            float pitch;
            float yaw;
            float distance;

            bool operator==(CameraState const & other) const
            {
                return lookAt.isApprox(other.lookAt, 1e-6f) &&
                       std::abs(pitch - other.pitch) < 1e-6f && std::abs(yaw - other.yaw) < 1e-6f &&
                       std::abs(distance - other.distance) < 1e-6f;
            }

            bool operator!=(CameraState const & other) const
            {
                return !(*this == other);
            }
        };

        CameraState m_lastCameraState;
        bool m_modelModifiedSinceLastCenter = false;
        float2 m_lastViewportSize{0, 0};
        bool m_viewportSizeChangedSinceLastCenter = false;

        // Helper methods for permanent centering
        void updateCameraStateTracking();
        bool shouldRecalculateCenter();
        CameraState getCurrentCameraState();
        void onCameraManuallyMoved();
        [[nodiscard]] bool isNeutralDocumentReady();

        async_rendering::AsyncRenderFeatureConfig m_asyncConfig{};
        async_rendering::NeutralRenderScheduler m_neutralRenderScheduler{
          [this](async_rendering::RenderTaskRequest const & task)
            -> std::optional<compute::RenderRequest> {
              if (!m_core || !m_renderBackendSession)
              {
                  return std::nullopt;
              }

              auto const width = task.width > 0u ? task.width : static_cast<std::uint32_t>(std::max(
                1.0f,
                std::clamp(m_renderWindowSize_px.x * m_renderWindowState.renderQuality,
                           1.0f,
                           16000.0f)));
              auto const height = task.height > 0u ? task.height : static_cast<std::uint32_t>(std::max(
                1.0f,
                std::clamp(m_renderWindowSize_px.y * m_renderWindowState.renderQuality,
                           1.0f,
                           16000.0f)));

              auto const firstRow = static_cast<std::uint32_t>(std::min(task.startLine,
                                                                          static_cast<std::size_t>(height)));
              auto const requestedLineCount = task.lineCount > 0u ? task.lineCount : height;
              auto const endRow = static_cast<std::uint32_t>(std::min(
                firstRow + requestedLineCount,
                static_cast<std::size_t>(height)));
              auto const viewport = compute::RenderViewport{.width = width,
                                                            .height = height,
                                                            .firstRow = firstRow,
                                                            .endRow = endRow};
              if (!viewport.isValid())
              {
                  return std::nullopt;
              }

              auto const resources = m_core->getResourceContext();
              if (!resources)
              {
                  return std::nullopt;
              }

              auto const freshness = compute::RenderFreshnessStamp{
                .sceneGeneration = task.stamp.sceneEpoch,
                .viewGeneration = task.stamp.viewEpoch,
                .parameterGeneration = task.stamp.parameterEpoch};

              try
              {
                  auto request = compute::OpenCLRenderRequestFactory::create(*resources,
                                                                              viewport,
                                                                              freshness);
                  if (auto const boundingBox = m_core->getBoundingBox(); boundingBox.has_value())
                  {
                      compute::RenderBounds const modelBounds{
                        .min = {boundingBox->min.x, boundingBox->min.y, boundingBox->min.z},
                        .max = {boundingBox->max.x, boundingBox->max.y, boundingBox->max.z}};
                      if (modelBounds.isValid())
                      {
                          request.modelBounds = modelBounds;
                      }
                  }
                  return request;
              }
              catch (...)
              {
                  return std::nullopt;
              }
          }};
        async_rendering::RenderWorkflowController & m_renderUpdateCoordinator{
          m_neutralRenderScheduler.workflow()};
        std::unique_ptr<compute::RenderBackendSession> m_renderBackendSession;
        std::unique_ptr<async_rendering::OpenGLFramePresenter> m_neutralFramePresenter;
        bool m_neutralBackendActive{false};
        std::uint64_t m_neutralSceneGeneration{0u};
        std::uint32_t m_neutralViewportWidth{0u};
        std::uint32_t m_neutralViewportHeight{0u};
        std::vector<async_rendering::RenderCommand> m_pendingRenderCommands;
        std::shared_ptr<async_rendering::AsyncRenderController> m_asyncController;
        std::atomic<uint64_t> m_asyncEpochCounter{0};
        std::atomic<uint64_t> m_asyncCurrentEpoch{0};
          std::atomic<uint64_t> m_asyncViewEpoch{0};
        std::atomic<uint64_t> m_asyncInFlightEpoch{0};
          std::atomic<uint64_t> m_asyncInFlightViewEpoch{0};
        std::atomic<uint64_t> m_asyncFrameCounter{0};
        std::atomic<bool> m_asyncJobInFlight{false};
        std::atomic<bool> m_asyncRealtimeJobInFlight{false};
        /// Set when a StaticFullFrameProbe job is in-flight; used to suppress the progressive
        /// buffer from display so the partial chunk content is not shown while the probe renders.
        std::atomic<bool> m_asyncStaticFullFrameJobInFlight{false};
        std::atomic<bool> m_asyncBboxJobInFlight{false};
        std::atomic<bool> m_asyncBboxUpdatePending{
          false}; // Tracks if bbox needs update after current job
        std::atomic<bool> m_asyncSdfJobInFlight{false};

        /// Debounce delay before recomputing a stale bounding box
        static constexpr auto kBboxDebounceDelay = std::chrono::milliseconds(1000);
        std::chrono::steady_clock::time_point m_lastParameterChangeTime{};
        std::atomic<uint64_t> m_asyncSdfInFlightEpoch{0};
        std::atomic<bool> m_lowResFeedbackPending{false};
        std::atomic<uint64_t> m_lastLowResPreviewEpoch{0};
        /// View epoch of the content currently held by the result image (the low-res preview
        /// that seeds the progressive HQ buffer). Used to avoid seeding an HQ frame from a
        /// stale-view preview, which would produce torn (mixed-view) progressive frames during
        /// rapid window resizing.
        std::atomic<uint64_t> m_lastLowResPreviewViewEpoch{0};
        bool m_asyncInitialized{false};
        bool m_compilationInvalidated{false};

        // Progressive rendering: reuse same buffer for all chunks in a frame
        async_rendering::FrameBuffer * m_asyncProgressiveBuffer{nullptr};
        std::atomic<uint64_t> m_asyncProgressiveEpoch{0};
        std::atomic<uint64_t> m_asyncProgressiveViewEpoch{0};
        /// GPU parameter-buffer generation the progressive buffer's HQ lines were rendered with.
        /// A mismatch against the live generation means a parameter upload landed mid-fill, so the
        /// buffer must be re-seeded to avoid a torn (old-params top / new-params bottom) frame.
        std::atomic<uint64_t> m_asyncProgressiveParamGeneration{0};

        // Async preview rendering state (separate from HQ progressive rendering)
        std::atomic<uint64_t> m_asyncPreviewEpoch{0};       ///< Current preview epoch for cancellation
        std::atomic<bool> m_asyncPreviewJobInFlight{false}; ///< True if preview job is executing
        std::atomic<uint64_t> m_asyncPreviewFrameId{0};     ///< Latest completed preview frame ID
        uint64_t m_asyncPreviewFrameCounter{0};             ///< Counter for generating unique frame IDs
        std::chrono::steady_clock::time_point m_asyncPreviewEnqueueTime{}; ///< For latency tracking

        // Streaming preview state (tight render loop during parameter drag)
        std::atomic<bool> m_streamingPreviewActive{false}; ///< True while streaming loop should run
        std::atomic<bool> m_streamingJobInFlight{false};   ///< True while streaming coroutine is executing
        std::atomic<bool> m_streamingFrameConsumed{true};  ///< Handshake: UI thread sets true after resample

        // Framebuffer preservation during resize (prevents flicker)
        bool m_preserveContentDuringResize{false}; ///< Keep displaying old texture during resize
        bool m_deferredResizePending{false}; ///< Buffer reallocation deferred until render completes
    };
}
