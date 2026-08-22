#pragma once

#include "InteractiveRenderPathPolicy.h"
#include "RealtimeRaymarchController.h"
#include "RenderUpdateTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gladius::ui::async_rendering
{
    enum class RenderCommandType
    {
        StartTask,
        PresentFrame,
        DiscardTaskResult,
        KeepCurrentFrame
    };

    struct RenderCommand
    {
        RenderCommandType type{RenderCommandType::KeepCurrentFrame};
        RenderTaskRequest task{};
        RenderTaskResult result{};
        RenderStamp stamp{};
        RenderStampMask presentationMask{RenderStampMask::displayFrame()};
    };

    struct RenderUpdateDecision
    {
        std::vector<RenderCommand> commands;
    };

    /**
     * @brief Pure render/update policy that converts semantic UI/render events into task commands.
     *
     * The coordinator intentionally has no ImGui, OpenGL, OpenCL, GLImageBuffer, or ComputeCore
     * dependency. Production code should translate these semantic commands into concrete work,
     * while unit tests can drive the full epoch/backpressure/stale-result behavior directly.
     */
    class RenderUpdateCoordinator
    {
      public:
        RenderUpdateCoordinator()
        {
            m_realtime.resetForResolution(m_width, m_height);
        }

        void configureRealtime(RealtimeRaymarchConfig config)
        {
            m_realtime.configure(config);
            m_realtime.resetForResolution(m_width, m_height);
        }

        [[nodiscard]] RealtimeRaymarchConfig const & realtimeConfig() const noexcept
        {
            return m_realtime.config();
        }

        [[nodiscard]] bool isRealtimeActive() const noexcept
        {
            return m_realtime.isRealtimeActive();
        }

        /// Returns true when realtime frame rendering is expected to succeed at interactive rates.
        /// This covers Force mode (always) and Auto mode once the controller has measured
        /// sufficient GPU performance.  Use this — rather than checking the mode directly —
        /// to gate the synchronous render path so Auto-mode users get the same low-latency
        /// feedback as Force-mode users.
        [[nodiscard]] bool isRealtimeSchedulingActive() const noexcept
        {
            return m_realtime.config().mode == RealtimeRaymarchMode::Force ||
                   (m_realtime.config().mode == RealtimeRaymarchMode::Auto &&
                    m_realtime.isRealtimeActive());
        }

        /// Returns true when Auto mode has latched the current interaction to the
        /// simpler preview path after a guard blocked exact realtime.  This latch is
        /// cleared when the gesture ends and prevents mid-gesture path switching.
        [[nodiscard]] bool isAutoPreviewFallbackActive() const noexcept
        {
            return m_realtime.config().mode == RealtimeRaymarchMode::Auto &&
                   m_interactionState != RenderInteractionState::Static &&
                   m_autoGestureLockedSimpler;
        }

        void setRealtimeGuards(RealtimeRaymarchGuards guards) noexcept
        {
            m_realtimeGuards = guards;
        }

        void recordStaticProgressiveSample(RealtimeRaymarchSample const & sample)
        {
            m_realtime.recordStaticProgressiveSample(sample);
        }

        void recordStaticFullFrameSample(RealtimeRaymarchSample const & sample)
        {
            m_realtime.recordStaticFullFrameSample(sample);
        }

        void recordInteractiveRealtimeSample(RealtimeRaymarchSample const & sample)
        {
            m_realtime.recordInteractiveRealtimeSample(sample);
        }

        void recordRealtimeRejectedAttempt()
        {
            m_realtime.recordRejectedAttempt();
        }

        [[nodiscard]] RenderStamp latestStamp() const noexcept
        {
            return m_latestStamp;
        }

        [[nodiscard]] RenderInteractionState interactionState() const noexcept
        {
            return m_interactionState;
        }

        [[nodiscard]] bool isBoundingBoxCurrent() const noexcept
        {
            return m_boundingBoxStamp.has_value() &&
                   matches(*m_boundingBoxStamp, m_latestStamp, RenderStampMask::heavyGeometryTask());
        }

        [[nodiscard]] bool isSdfCurrent() const noexcept
        {
            return m_sdfStamp.has_value() &&
                   matches(*m_sdfStamp, m_latestStamp, RenderStampMask::heavyGeometryTask());
        }

        [[nodiscard]] bool isHeavyGeometryCurrent() const noexcept
        {
            return isBoundingBoxCurrent() && isSdfCurrent();
        }

        [[nodiscard]] bool isHighQualityFrameCurrent() const noexcept
        {
            return m_highQualityFrameStamp.has_value() &&
                   matches(*m_highQualityFrameStamp, m_latestStamp, RenderStampMask::displayFrame());
        }

        [[nodiscard]] RenderUpdateDecision configureViewport(uint32_t width, uint32_t height)
        {
            RenderUpdateDecision decision{};
            if (width == 0 || height == 0 || (width == m_width && height == m_height))
            {
                return decision;
            }

            m_width = width;
            m_height = height;
            ++m_latestStamp.viewportEpoch;
            m_realtime.resetForResolution(m_width, m_height);
            scheduleStaticCatchUp(decision);
            return decision;
        }

        /// @brief Notify the workflow that the camera changed.
        /// @param replaceStaleInteractiveInFlight Allow uncancellable backends to replace an
        ///        interactive frame on every camera update while retaining the backend submission
        ///        until its terminal callback.
        [[nodiscard]] RenderUpdateDecision notifyCameraChanged(bool replaceStaleInteractiveInFlight = false)
        {
            RenderUpdateDecision decision{};
            auto const previousState = m_interactionState;
            ++m_latestStamp.viewEpoch;
            if (replaceStaleInteractiveInFlight || previousState != RenderInteractionState::CameraInteracting)
            {
                releaseStaleInteractiveInFlight();
            }
            m_interactionState = RenderInteractionState::CameraInteracting;
            scheduleInteractiveFrame(decision);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyCameraInteractionEnded()
        {
            RenderUpdateDecision decision{};
            if (m_interactionState == RenderInteractionState::CameraInteracting)
            {
                m_interactionState = RenderInteractionState::Static;
            }
            m_autoGestureLockedSimpler = false;
            scheduleStaticCatchUp(decision);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyParameterChanged(bool interactionActive)
        {
            RenderUpdateDecision decision{};
            auto const previousState = m_interactionState;
            ++m_latestStamp.parameterEpoch;
            resetRealtimeLearning();
            m_parameterUploadStamp.reset();
            m_boundingBoxStamp.reset();
            m_sdfStamp.reset();
            m_interactionState = interactionActive ? RenderInteractionState::ParameterInteracting
                                                   : RenderInteractionState::Static;
            if (interactionActive && previousState != RenderInteractionState::ParameterInteracting)
            {
                releaseStaleInteractiveInFlight();
            }
            startTask(decision, RenderTaskType::ParameterUpload, m_latestStamp);
            if (interactionActive)
            {
                scheduleInteractiveFrame(decision);
            }
            else
            {
                scheduleStaticCatchUp(decision);
            }
            return decision;
        }

        /// Notify the workflow that parameter values were rebuilt into the materialized scene.
        /// Backends with embedded parameters do not need a separate upload task.
        [[nodiscard]] RenderUpdateDecision notifyEmbeddedParameterChanged(bool interactionActive)
        {
            RenderUpdateDecision decision{};
            // The WebGPU scene embeds parameter values and is immutable after materialization.
            // Rebuilding it therefore creates a new scene generation as well as a new parameter
            // generation; both must match the backend session and submitted frame freshness.
            ++m_latestStamp.sceneEpoch;
            ++m_latestStamp.parameterEpoch;
            resetRealtimeLearning();
            m_parameterUploadStamp = m_latestStamp;
            m_boundingBoxStamp.reset();
            m_sdfStamp.reset();
            m_interactionState = interactionActive ? RenderInteractionState::ParameterInteracting
                                                   : RenderInteractionState::Static;
            // Embedded-parameter backends can submit the replacement scene immediately while the
            // backend retains the old submission for resource lifetime. Drop every stale
            // coordinator-side interactive record on each edit; retaining one from an earlier
            // edit would make the type-only backpressure guard reject all later replacements.
            releaseStaleInteractiveInFlight();
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyParameterInteractionEnded()
        {
            RenderUpdateDecision decision{};
            m_interactionState = RenderInteractionState::Static;
            m_autoGestureLockedSimpler = false;
            scheduleStaticCatchUp(decision);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyStructuralModelChanged()
        {
            RenderUpdateDecision decision{};
            ++m_latestStamp.sceneEpoch;
            ++m_latestStamp.parameterEpoch;
            resetRealtimeLearning();
            m_programStamp.reset();
            m_parameterUploadStamp.reset();
            m_boundingBoxStamp.reset();
            m_sdfStamp.reset();
            m_interactionState = RenderInteractionState::Static;
            scheduleStaticCatchUp(decision);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyProgramCompilationCompleted()
        {
            return completeTask(RenderTaskResult{.type = RenderTaskType::ProgramCompilation,
                                                .stamp = m_latestStamp,
                                                .status = RenderTaskStatus::Completed});
        }

        [[nodiscard]] RenderUpdateDecision tick()
        {
            RenderUpdateDecision decision{};
            m_realtime.beginFrame();
            // Self-heal: reclaim any in-flight task the translation layer failed to complete and
            // that is already stale, so a single orphaned entry cannot wedge the catch-up pipeline.
            reconcileStaleInFlight();
            if (m_interactionState == RenderInteractionState::Static)
            {
                scheduleStaticCatchUp(decision);
            }
            else
            {
                if (m_interactionState == RenderInteractionState::ParameterInteracting &&
                    !areParametersUploaded())
                {
                    startTask(decision, RenderTaskType::ParameterUpload, m_latestStamp);
                }
                scheduleInteractiveFrame(decision);
            }
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision startDisplayTask(RenderTaskType type,
                                                             size_t lineCount = 0)
        {
            RenderUpdateDecision decision{};
            startTask(decision, type, m_latestStamp, lineCount);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision completeTask(RenderTaskResult const & result,
                                                        bool scheduleFollowUp = true)
        {
            RenderUpdateDecision decision{};
            removeInFlight(result);

            auto const mask = freshnessMaskFor(result.type);
            bool const current = result.isCurrentFor(m_latestStamp, mask);
            bool const cameraCompatibleDisplay =
              result.succeeded() && result.producedDisplayFrame &&
              (result.type == RenderTaskType::RealtimeFullFrame ||
               result.type == RenderTaskType::LowResolutionPreview ||
               result.type == RenderTaskType::StreamingPreview) &&
              isCameraCompatible(result.stamp, m_latestStamp);
            if (!current && !cameraCompatibleDisplay)
            {
                decision.commands.push_back(RenderCommand{.type = RenderCommandType::DiscardTaskResult,
                                                          .result = result,
                                                          .stamp = m_latestStamp});
                return decision;
            }

            switch (result.type)
            {
            case RenderTaskType::ProgramCompilation:
                m_programStamp = result.stamp;
                break;
            case RenderTaskType::ParameterUpload:
                m_parameterUploadStamp = result.stamp;
                break;
            case RenderTaskType::BoundingBoxUpdate:
                m_boundingBoxStamp = result.stamp;
                break;
            case RenderTaskType::SdfPrecomputation:
                m_sdfStamp = result.stamp;
                break;
            case RenderTaskType::RealtimeFullFrame:
            case RenderTaskType::StaticFullFrameProbe:
            case RenderTaskType::ProgressiveHighQualityChunk:
            case RenderTaskType::LowResolutionPreview:
            case RenderTaskType::StreamingPreview:
                if (result.producedDisplayFrame)
                {
                    if (current && result.completedFrame &&
                        (result.type == RenderTaskType::ProgressiveHighQualityChunk ||
                         result.type == RenderTaskType::RealtimeFullFrame ||
                         result.type == RenderTaskType::StaticFullFrameProbe))
                    {
                        m_highQualityFrameStamp = result.stamp;
                    }
                    decision.commands.push_back(RenderCommand{.type = RenderCommandType::PresentFrame,
                                                              .result = result,
                                                              .stamp = result.stamp,
                                                              .presentationMask = current
                                                                                   ? RenderStampMask::displayFrame()
                                                                                   : RenderStampMask::cameraCompatibleFrame()});
                }
                break;
            }

            if (scheduleFollowUp && m_interactionState == RenderInteractionState::Static)
            {
                scheduleStaticCatchUp(decision);
            }
            return decision;
        }

      private:
        struct InFlightTask
        {
            uint64_t requestId{0};
            RenderTaskType type{RenderTaskType::ProgressiveHighQualityChunk};
            RenderStamp stamp{};
        };

        [[nodiscard]] static constexpr RenderStampMask freshnessMaskFor(RenderTaskType type) noexcept
        {
            switch (type)
            {
            case RenderTaskType::ProgramCompilation:
                return RenderStampMask::sceneOnly();
            case RenderTaskType::ParameterUpload:
            case RenderTaskType::BoundingBoxUpdate:
            case RenderTaskType::SdfPrecomputation:
                return RenderStampMask::heavyGeometryTask();
            case RenderTaskType::RealtimeFullFrame:
            case RenderTaskType::StaticFullFrameProbe:
            case RenderTaskType::ProgressiveHighQualityChunk:
            case RenderTaskType::LowResolutionPreview:
            case RenderTaskType::StreamingPreview:
            default:
                return RenderStampMask::displayFrame();
            }
        }

        [[nodiscard]] bool hasAnyInFlight(RenderTaskType type) const noexcept
        {
            return std::any_of(m_inFlight.begin(),
                               m_inFlight.end(),
                               [type](InFlightTask const & task) { return task.type == type; });
        }

        [[nodiscard]] bool hasInteractiveFrameInFlight() const noexcept
        {
            return hasEquivalentInFlight(RenderTaskType::RealtimeFullFrame, m_latestStamp) ||
                   hasEquivalentInFlight(RenderTaskType::LowResolutionPreview, m_latestStamp) ||
                   hasEquivalentInFlight(RenderTaskType::StreamingPreview, m_latestStamp);
        }

        void releaseStaleInteractiveInFlight()
        {
            auto const oldEnd = m_inFlight.end();
            auto const newEnd = std::remove_if(m_inFlight.begin(),
                                               m_inFlight.end(),
                                               [this](InFlightTask const & task)
                                               {
                                                   bool const interactiveFrameTask =
                                                     task.type == RenderTaskType::RealtimeFullFrame ||
                                                     task.type == RenderTaskType::LowResolutionPreview ||
                                                     task.type == RenderTaskType::StreamingPreview;
                                                   return interactiveFrameTask &&
                                                          !matches(task.stamp,
                                                                   m_latestStamp,
                                                                   RenderStampMask::displayFrame());
                                               });
            m_inFlight.erase(newEnd, oldEnd);
        }

        /// Drops every in-flight heavy/static task whose stamp is already stale under that task's
        /// own freshness mask, i.e. a task whose eventual result would be discarded by
        /// completeTask() anyway.
        ///
        /// This is the self-healing counterpart to the strict StartTask/completeTask pairing the
        /// translation layer (RenderWindow) must otherwise guarantee across many early-return and
        /// atomic code paths. If any one of those paths ever fails to drive a started task back to a
        /// terminal completeTask()/failTask(), the orphaned entry would block all future tasks of
        /// that type forever (hasAnyInFlight() is stamp-agnostic), stalling the static catch-up
        /// pipeline. Reclaiming stale entries at every tick keeps the scheduler from getting wedged.
        /// It is safe because a stale task can never become current again, and the translation layer
        /// guards in-flight GPU jobs separately, so reclaiming here cannot cause duplicate execution.
        ///
        /// Interactive frame tasks (RealtimeFullFrame / LowResolutionPreview / StreamingPreview) are
        /// intentionally excluded: stale interactive frames are retained as backpressure so only one
        /// interactive frame is ever in flight (latest-wins on completion). Those are reclaimed by
        /// releaseStaleInteractiveInFlight() at the appropriate interaction transitions instead.
        void reconcileStaleInFlight()
        {
            auto const oldEnd = m_inFlight.end();
            auto const newEnd = std::remove_if(m_inFlight.begin(),
                                               m_inFlight.end(),
                                               [this](InFlightTask const & task)
                                               {
                                                   bool const interactiveFrameTask =
                                                     task.type == RenderTaskType::RealtimeFullFrame ||
                                                     task.type == RenderTaskType::LowResolutionPreview ||
                                                     task.type == RenderTaskType::StreamingPreview;
                                                   return !interactiveFrameTask &&
                                                          !matches(task.stamp,
                                                                   m_latestStamp,
                                                                   freshnessMaskFor(task.type));
                                               });
            m_inFlight.erase(newEnd, oldEnd);
        }

        [[nodiscard]] bool hasEquivalentInFlight(RenderTaskType type, RenderStamp const & stamp) const noexcept
        {
            auto const mask = freshnessMaskFor(type);
            return std::any_of(m_inFlight.begin(),
                               m_inFlight.end(),
                               [type, stamp, mask](InFlightTask const & task)
                               { return task.type == type && matches(task.stamp, stamp, mask); });
        }

        [[nodiscard]] bool isProgramCurrent() const noexcept
        {
            return m_programStamp.has_value() &&
                   matches(*m_programStamp, m_latestStamp, RenderStampMask::sceneOnly());
        }

        [[nodiscard]] bool areParametersUploaded() const noexcept
        {
            return m_parameterUploadStamp.has_value() &&
                   matches(*m_parameterUploadStamp, m_latestStamp, RenderStampMask::heavyGeometryTask());
        }

        void startTask(RenderUpdateDecision & decision,
                       RenderTaskType type,
                       RenderStamp const & stamp,
                       size_t lineCount = 0)
        {
            if (hasAnyInFlight(type) || hasEquivalentInFlight(type, stamp))
            {
                keepCurrentFrame(decision);
                return;
            }

            uint32_t requestWidth = m_width;
            uint32_t requestHeight = m_height;
            if (type == RenderTaskType::LowResolutionPreview)
            {
                requestWidth = std::max(1u, (m_width + 1u) / 2u);
                requestHeight = std::max(1u, (m_height + 1u) / 2u);
            }

            size_t const requestedLines = lineCount == 0 ? static_cast<size_t>(requestHeight) : lineCount;
            size_t const clampedLineCount =
              std::clamp(requestedLines, size_t{1}, static_cast<size_t>(requestHeight));

            RenderTaskRequest request{.requestId = m_nextRequestId++,
                                      .type = type,
                                      .stamp = stamp,
                                      .width = requestWidth,
                                      .height = requestHeight,
                                      .startLine = 0,
                                      .lineCount = clampedLineCount};
            m_inFlight.push_back(InFlightTask{.requestId = request.requestId,
                                              .type = request.type,
                                              .stamp = request.stamp});
            decision.commands.push_back(RenderCommand{.type = RenderCommandType::StartTask,
                                                      .task = request,
                                                      .stamp = stamp});
        }

        void keepCurrentFrame(RenderUpdateDecision & decision) const
        {
            decision.commands.push_back(RenderCommand{.type = RenderCommandType::KeepCurrentFrame,
                                                      .stamp = m_latestStamp});
        }

        void resetRealtimeLearning()
        {
            m_realtime.reset();
            m_realtime.resetForResolution(m_width, m_height);
            m_autoGestureLockedSimpler = false;
        }

        [[nodiscard]] bool autoModeAdmitsRealtime() const noexcept
        {
            return m_realtime.isRealtimeActive();
        }

        void scheduleInteractiveFrame(RenderUpdateDecision & decision)
        {
            auto const mode = m_realtime.config().mode;

            bool exactRealtimeAllowed = false;
            if (mode == RealtimeRaymarchMode::Force)
            {
                exactRealtimeAllowed =
                  m_realtime.canAttemptRealtime(m_width, m_height, m_realtimeGuards);
            }
            else if (mode == RealtimeRaymarchMode::Auto)
            {
                exactRealtimeAllowed = m_realtime.guardsAllowAttempt(m_realtimeGuards);
            }

            auto const path = chooseInteractiveRenderPath(
              InteractiveRenderPathInput{.mode = mode,
                                         .interactionState = m_interactionState,
                                         .interactiveFrameAlreadyInFlight = hasInteractiveFrameInFlight(),
                                         .autoParameterExactRealtimeActive = m_realtime.isRealtimeActive(),
                                         .autoInteractiveExactRealtimeAdmitted = autoModeAdmitsRealtime(),
                                         .preferSimplerPreview = m_autoGestureLockedSimpler,
                                         .exactRealtimeAllowed = exactRealtimeAllowed});

            if (mode == RealtimeRaymarchMode::Auto &&
                m_interactionState != RenderInteractionState::Static &&
                path == InteractiveRenderPath::LowResolutionPreview)
            {
                m_autoGestureLockedSimpler = true;
            }

            switch (path)
            {
            case InteractiveRenderPath::ExactRealtime:
                startTask(decision, RenderTaskType::RealtimeFullFrame, m_latestStamp);
                return;
            case InteractiveRenderPath::LowResolutionPreview:
                startTask(decision, RenderTaskType::LowResolutionPreview, m_latestStamp);
                return;
            case InteractiveRenderPath::KeepCurrentFrame:
            default:
                keepCurrentFrame(decision);
                return;
            }
        }

        void scheduleStaticCatchUp(RenderUpdateDecision & decision)
        {
            if (!isProgramCurrent())
            {
                startTask(decision, RenderTaskType::ProgramCompilation, m_latestStamp);
                return;
            }
            if (!areParametersUploaded())
            {
                startTask(decision, RenderTaskType::ParameterUpload, m_latestStamp);
                return;
            }
            if (!isBoundingBoxCurrent())
            {
                startTask(decision, RenderTaskType::BoundingBoxUpdate, m_latestStamp);
                return;
            }
            if (!isSdfCurrent())
            {
                startTask(decision, RenderTaskType::SdfPrecomputation, m_latestStamp);
                return;
            }
            if (isHighQualityFrameCurrent())
            {
                keepCurrentFrame(decision);
                return;
            }
            if (m_realtime.canAttemptStaticFullFrame(m_width, m_height, m_realtimeGuards))
            {
                startTask(decision, RenderTaskType::StaticFullFrameProbe, m_latestStamp);
                return;
            }
            startTask(decision, RenderTaskType::ProgressiveHighQualityChunk, m_latestStamp);
        }

        void removeInFlight(RenderTaskResult const & result)
        {
            auto const byRequestId = [result](InFlightTask const & task)
            { return result.requestId != 0 && task.requestId == result.requestId; };
            auto const byTypeAndStamp = [result](InFlightTask const & task)
            {
                return task.type == result.type &&
                       matches(task.stamp, result.stamp, freshnessMaskFor(result.type));
            };
            auto const byTypeWithoutRequestId = [result](InFlightTask const & task)
            { return result.requestId == 0 && task.type == result.type; };

            auto const oldEnd = m_inFlight.end();
            auto const newEnd = std::remove_if(m_inFlight.begin(),
                                               m_inFlight.end(),
                                               [byRequestId, byTypeAndStamp, byTypeWithoutRequestId](
                                                 InFlightTask const & task)
                                               {
                                                   return byRequestId(task) || byTypeAndStamp(task) ||
                                                          byTypeWithoutRequestId(task);
                                               });
            m_inFlight.erase(newEnd, oldEnd);
        }

        RenderStamp m_latestStamp{};
        std::optional<RenderStamp> m_programStamp{m_latestStamp};
        std::optional<RenderStamp> m_parameterUploadStamp{m_latestStamp};
        std::optional<RenderStamp> m_boundingBoxStamp{};
        std::optional<RenderStamp> m_sdfStamp{};
        std::optional<RenderStamp> m_highQualityFrameStamp{};
        std::vector<InFlightTask> m_inFlight;
        RealtimeRaymarchController m_realtime;
        RealtimeRaymarchGuards m_realtimeGuards{};
        RenderInteractionState m_interactionState{RenderInteractionState::Static};
        uint64_t m_nextRequestId{1};
        uint32_t m_width{1};
        uint32_t m_height{1};
        /// Per-gesture latch: set when Auto mode first decides to use simpler rendering during
        /// an interaction. Cleared when the gesture ends to prevent mid-gesture quality switches.
        bool m_autoGestureLockedSimpler{false};
    };
}
