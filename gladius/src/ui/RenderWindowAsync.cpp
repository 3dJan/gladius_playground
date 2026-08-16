#include "RenderWindow.h"

#include "ExportState.h"
#include "Profiling.h"
#include "render/RenderModeUpdatePolicy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace gladius::ui
{
    void RenderWindow::renderAsync(RenderWindowState & state)
    {
        ProfileFunction;
        constexpr std::size_t kInitialProgressiveStepSize = 8;

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
        bool screenResizeRequired =
          resultImage && (static_cast<int>(resultImage->getWidth()) != desiredScreenWidth ||
                          static_cast<int>(resultImage->getHeight()) != desiredScreenHeight);

        // Treat render-target resize as a scheduling barrier: the pure coordinator's
        // viewport stamp must not be advanced until the concrete CL/GL images have the
        // matching size. Otherwise an old-size render job can complete with the new
        // viewport stamp, making HQ/static catch-up and Auto realtime learning believe
        // the resized viewport is already current.
        bool const shouldDeferResize = m_preserveContentDuringResize && m_deferredResizePending;
        bool const hqRenderInFlight = m_asyncJobInFlight.load(std::memory_order_acquire);
        if (!shouldDeferResize && screenResizeRequired)
        {
            if (hqRenderInFlight)
            {
                m_dirty = true;
                m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
                state.isRendering = false;
                return;
            }

            if (m_asyncController && m_asyncProgressiveBuffer != nullptr)
            {
                [[maybe_unused]] bool const released =
                  m_asyncController->tryTransitionBuffer(m_asyncProgressiveBuffer,
                                                         async_rendering::FrameState::Writing,
                                                         async_rendering::FrameState::Idle);
                m_asyncProgressiveBuffer = nullptr;
                m_asyncProgressiveEpoch.store(0, std::memory_order_release);
                m_asyncProgressiveViewEpoch.store(0, std::memory_order_release);
                state.currentLine = 0;
                state.isRendering = false;
            }

            bool const resized = m_core->setScreenResolution(desiredScreenWidth, desiredScreenHeight);
            auto const latestImage = m_core->getResultImage();
            bool const resizeStillRequired =
              !latestImage || static_cast<int>(latestImage->getWidth()) != desiredScreenWidth ||
              static_cast<int>(latestImage->getHeight()) != desiredScreenHeight;
            if (resizeStillRequired)
            {
                // setScreenResolution can legitimately fail while another compute operation
                // holds the token. Keep the old frame visible and retry next UI frame instead
                // of scheduling work against stale dimensions.
                m_dirty = true;
                m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
                state.isRendering = false;
                return;
            }

            screenResizeRequired = false;
            if (resized)
            {
                invalidateView();
            }
        }

        // Clear preserve flags once rendering may resume. The early render() guard normally
        // handles this on the first stable-size frame; this branch is a defensive fallback.
        if (shouldDeferResize && m_forceLowResRenderOnNextFrame.load(std::memory_order_acquire))
        {
            m_preserveContentDuringResize = false;
            m_deferredResizePending = false;
        }

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

            // Defer transition from CameraInteracting to Static state until camera has been idle
            // for at least 1 second. This prevents HQ progressive rendering from starting and
            // getting aborted repeatedly during stop-and-go camera movements.
            if (!state.isMoving &&
                m_renderUpdateCoordinator.interactionState() ==
                  async_rendering::RenderInteractionState::CameraInteracting)
            {
                auto const now = std::chrono::system_clock::now();
                if (m_lastCameraIdleTime == TimeStamp{})
                {
                    // First frame idle — record the time
                    m_lastCameraIdleTime = now;
                }
                else
                {
                    auto const timeSinceIdle = now - m_lastCameraIdleTime;
                    if (timeSinceIdle >= std::chrono::seconds(1))
                    {
                        queueRenderDecision(m_renderUpdateCoordinator.notifyCameraInteractionEnded());
                        m_lastCameraIdleTime = TimeStamp{}; // Reset for next interaction
                    }
                }
            }

            // Parameter-drag settle: once no parameter change has arrived for the debounce
            // window, end the parameter interaction so the coordinator's static catch-up
            // (bbox -> SDF -> high-quality) resumes and the static view shows full detail.
            //
            // This MUST run here, before the early-returning executeQueuedRenderCommands()
            // below. In Auto mode that is not yet proven fast (and in any streaming-preview
            // case) an interactive low-res preview frame is scheduled on every tick, so
            // executeQueuedRenderCommands() returns true and renderAsync() returns early.
            // The legacy settle path further down would then never be reached, leaving the
            // view stuck on the low-res preview until the next camera move forces a
            // notifyCameraInteractionEnded() transition. Driving the settle from here makes
            // the transition independent of which interactive feedback path is active.
            bool const paramInteracting =
              m_renderUpdateCoordinator.interactionState() ==
              async_rendering::RenderInteractionState::ParameterInteracting;
            bool const bboxStaleForSettle = m_core->isBoundingBoxStale();
            bool const bboxJobForSettle = m_asyncBboxJobInFlight.load(std::memory_order_acquire);
            bool const sdfJobForSettle = m_asyncSdfJobInFlight.load(std::memory_order_acquire);
            auto const sinceParamChange =
              std::chrono::steady_clock::now() - m_lastParameterChangeTime;
            bool const debounceElapsedForSettle = sinceParamChange >= kBboxDebounceDelay;

            if (paramInteracting && bboxStaleForSettle && !bboxJobForSettle && !sdfJobForSettle &&
                debounceElapsedForSettle)
            {
                // Parameter drag has ended. This is a semantic interaction transition,
                // independent of whether the active feedback path used streaming preview
                // (Force mode does not start streaming preview at all).
                finishParameterInteraction();

                m_core->recomputeStaleBoundingBox();
                m_core->setSdfValid(false);
                m_preComputedSdfDirty.store(true, std::memory_order_release);

                // Bump epoch to invalidate the stale HQ/preview front buffer from before the
                // drag so the fresh bbox -> SDF -> HQ pipeline starts clean.
                notifyAsyncEpochIncrement();
                m_suppressHQDisplay.store(false, std::memory_order_release);
            }

            queueRenderDecision(m_renderUpdateCoordinator.tick());
            bool const queuedWorkStarted = executeQueuedRenderCommands(state);

            if (queuedWorkStarted)
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
            // tick. Computing a new SDF only to have it immediately discarded wastes
            // GPU time. Low-res preview already works via direct function evaluation,
            // so we can safely wait until the drag stops (same debounce as bbox).
            bool const bboxStale = m_core->isBoundingBoxStale();
            bool const sdfDebounceElapsed =
              !bboxStale ||
              (std::chrono::steady_clock::now() - m_lastParameterChangeTime >= kBboxDebounceDelay);

            // Don't schedule SDF precomputation while the camera is actively moving in
            // realtime mode: the realtime renderer evaluates SDF on-the-fly and doesn't
            // need the precomputed version. Starting the SDF job now would set
            // hardBlocker=true, preventing canAttemptRealtime() from returning true for
            // the entire SDF compute duration. The coordinator will schedule SDF via
            // scheduleStaticCatchUp() once notifyCameraInteractionEnded() fires.
            bool const cameraInteracting =
              m_renderUpdateCoordinator.interactionState() ==
              async_rendering::RenderInteractionState::CameraInteracting;
            bool const isRealtimeForSdfGuard =
              m_renderUpdateCoordinator.realtimeConfig().mode !=
              async_rendering::RealtimeRaymarchMode::Off;
            bool const suppressSdfDuringCameraInteraction =
              cameraInteracting && isRealtimeForSdfGuard;

            if (sdfDirty && !sdfJobActive && sdfDebounceElapsed && !suppressSdfDuringCameraInteraction)
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

            // Note: the parameter-drag settle transition (finishParameterInteraction +
            // stale-bbox recompute + epoch bump) now runs earlier, in the coordinator block
            // above, so it cannot be skipped by the early return when an interactive preview
            // frame is scheduled. See the ParameterInteracting settle block in this function.

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

            (void) updateCameraCentering();

        if (state.isMoving && m_preComputedSdfDirty)
        {
            m_core->getResourceContext()->getRenderingSettings().approximation = AM_FULL_MODEL;
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

        bool const shouldUseLowResPreview = async_rendering::shouldUseLegacyLowResPreview(
          async_rendering::LegacyLowResPreviewInput{
            .mode = m_renderUpdateCoordinator.realtimeConfig().mode,
            .interactionState = m_renderUpdateCoordinator.interactionState(),
            .autoRealtimeActive = m_renderUpdateCoordinator.isRealtimeActive(),
            .stateIsMoving = state.isMoving,
            .forceLowResRender = forceLowResRender,
            .lowResFeedbackPending = lowResPending,
            .exactRealtimeJobInFlight = m_asyncRealtimeJobInFlight.load(std::memory_order_acquire)});

        if (shouldUseLowResPreview)
        {
            bool const hadActiveProgressive =
              state.isRendering || m_asyncJobInFlight.load(std::memory_order_acquire);

            if (lowResPending && hadActiveProgressive &&
                !m_streamingPreviewActive.load(std::memory_order_acquire))
            {
                // Do not bump the global epoch here.
                //
                // lowResPending is also used by adaptive preview quality tuning and
                // camera/preview handoffs. Incrementing the epoch on every such handoff
                // repeatedly invalidates HQ work and can cause visible restart churn.
                //
                // Semantic invalidation (parameter/model changes) already bumps epoch in
                // dedicated paths (e.g. settle/finish transitions). Here we only reset the
                // local progressive state so preview can take over without forcing another
                // global invalidation cycle.
                state.currentLine = 0;
                state.renderingStepSize = kInitialProgressiveStepSize;
                state.isRendering = false;
            }

            // Decide whether to use async or sync preview:
            // - Async preview keeps the UI thread non-blocking for smooth parameter drag
            // - Sync preview is the fallback when async is unavailable
            // During parameter drag, SDF jobs are debounced so there's no GPU contention.
            bool const streamingActive = m_streamingPreviewActive.load(std::memory_order_acquire);
            bool const useAsyncPreview = !previewJobInFlight;

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
        auto const realtimeMode = m_renderUpdateCoordinator.realtimeConfig().mode;
        bool const preferExactRealtime =
          realtimeMode == async_rendering::RealtimeRaymarchMode::Force ||
          m_renderUpdateCoordinator.isRealtimeActive();
        bool const useLowResPreviewBeforeHq = !preferExactRealtime;

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

        if (!m_asyncJobInFlight.load(std::memory_order_acquire) &&
            !m_lowResFeedbackPending.load(std::memory_order_acquire))
        {
            state.isRendering = true;
            if (!scheduleAsyncRenderJob(state))
            {
                state.isRendering = false;
            }
        }
    }
}
