#pragma once

#include "RealtimeRaymarchController.h"
#include "RenderUpdateTypes.h"

#include <algorithm>
#include <iostream>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gladius::ui::async_rendering
{
    enum class RenderInteractionState
    {
        Static,
        CameraInteracting,
        ParameterInteracting
    };

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

        [[nodiscard]] RenderUpdateDecision notifyCameraChanged()
        {
            RenderUpdateDecision decision{};
            ++m_latestStamp.viewEpoch;
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
            scheduleStaticCatchUp(decision);
            return decision;
        }

        [[nodiscard]] RenderUpdateDecision notifyParameterChanged(bool interactionActive)
        {
            RenderUpdateDecision decision{};
            ++m_latestStamp.parameterEpoch;
            resetRealtimeLearning();
            m_parameterUploadStamp.reset();
            m_boundingBoxStamp.reset();
            m_sdfStamp.reset();
            m_interactionState = interactionActive ? RenderInteractionState::ParameterInteracting
                                                   : RenderInteractionState::Static;

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

        [[nodiscard]] RenderUpdateDecision notifyParameterInteractionEnded()
        {
            RenderUpdateDecision decision{};
            m_interactionState = RenderInteractionState::Static;
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

        [[nodiscard]] RenderUpdateDecision completeTask(RenderTaskResult const & result)
        {
            RenderUpdateDecision decision{};
            removeInFlight(result);

            auto const mask = freshnessMaskFor(result.type);
            if (!result.isCurrentFor(m_latestStamp, mask))
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
            case RenderTaskType::ProgressiveHighQualityChunk:
            case RenderTaskType::LowResolutionPreview:
            case RenderTaskType::StreamingPreview:
                if (result.producedDisplayFrame)
                {
                    if (result.completedFrame &&
                        (result.type == RenderTaskType::ProgressiveHighQualityChunk ||
                         result.type == RenderTaskType::RealtimeFullFrame))
                    {
                        m_highQualityFrameStamp = result.stamp;
                    }
                    decision.commands.push_back(RenderCommand{.type = RenderCommandType::PresentFrame,
                                                              .result = result,
                                                              .stamp = result.stamp});
                }
                break;
            }

            if (m_interactionState == RenderInteractionState::Static)
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
            return hasAnyInFlight(RenderTaskType::RealtimeFullFrame) ||
                   hasAnyInFlight(RenderTaskType::LowResolutionPreview) ||
                   hasAnyInFlight(RenderTaskType::StreamingPreview);
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

        [[nodiscard]] bool isHighQualityFrameCurrent() const noexcept
        {
            return m_highQualityFrameStamp.has_value() &&
                   matches(*m_highQualityFrameStamp, m_latestStamp, RenderStampMask::displayFrame());
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

            size_t const requestedLines = lineCount == 0 ? static_cast<size_t>(m_height) : lineCount;
            size_t const clampedLineCount =
                std::clamp(requestedLines, size_t{1}, static_cast<size_t>(m_height));

            RenderTaskRequest request{.requestId = m_nextRequestId++,
                                      .type = type,
                                      .stamp = stamp,
                                      .width = m_width,
                                      .height = m_height,
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
            std::cout << "[RT Coord] resetRealtimeLearning (Parameter/Scene/Resolution changed)\n";
            m_realtime.reset();
            m_realtime.resetForResolution(m_width, m_height);
        }

        void scheduleInteractiveFrame(RenderUpdateDecision & decision)
        {
            if (hasInteractiveFrameInFlight())
            {
                keepCurrentFrame(decision);
                return;
            }

            if (m_interactionState == RenderInteractionState::ParameterInteracting)
            {
                startTask(decision, RenderTaskType::LowResolutionPreview, m_latestStamp);
                return;
            }

            if (m_realtime.isRealtimeActive())
            {
                if (m_realtime.canAttemptRealtime(m_width, m_height, m_realtimeGuards))
                {
                    startTask(decision, RenderTaskType::RealtimeFullFrame, m_latestStamp);
                    return;
                }

                keepCurrentFrame(decision);
                return;
            }

            if (m_realtime.canAttemptRealtime(m_width, m_height, m_realtimeGuards))
            {
                startTask(decision, RenderTaskType::RealtimeFullFrame, m_latestStamp);
                return;
            }

            startTask(decision, RenderTaskType::LowResolutionPreview, m_latestStamp);
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
                startTask(decision, RenderTaskType::RealtimeFullFrame, m_latestStamp);
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
    };
}
