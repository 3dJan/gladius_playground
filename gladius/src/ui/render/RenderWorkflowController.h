#pragma once

#include "PresentedFrameLedger.h"
#include "RenderUpdateCoordinator.h"

#include <optional>
#include <utility>
#include <vector>

namespace gladius::ui::async_rendering
{
    struct RenderWorkflowDecision
    {
        std::vector<RenderCommand> commands;
        std::vector<FramePresentationCandidate> acceptedFrames;
        std::vector<FramePresentationCandidate> rejectedFrames;
        bool presentedFrameChanged{false};
    };

    /**
     * @brief Pure render workflow facade used to test user-visible frame decisions.
     *
     * The workflow controller wraps the semantic RenderUpdateCoordinator and adds the
     * missing presentation ledger: completed display tasks become frame candidates, and
     * candidates are accepted only if they are fresh for the latest display stamp and do
     * not downgrade an already-current higher-quality frame.
     */
    class RenderWorkflowController
    {
      public:
        void configureRealtime(RealtimeRaymarchConfig config)
        {
            m_coordinator.configureRealtime(config);
        }

        [[nodiscard]] RealtimeRaymarchConfig const & realtimeConfig() const noexcept
        {
            return m_coordinator.realtimeConfig();
        }

        void setRealtimeGuards(RealtimeRaymarchGuards guards) noexcept
        {
            m_coordinator.setRealtimeGuards(guards);
        }

        void recordStaticProgressiveSample(RealtimeRaymarchSample const & sample)
        {
            m_coordinator.recordStaticProgressiveSample(sample);
        }

        void recordStaticFullFrameSample(RealtimeRaymarchSample const & sample)
        {
            m_coordinator.recordStaticFullFrameSample(sample);
        }

        void recordInteractiveRealtimeSample(RealtimeRaymarchSample const & sample)
        {
            m_coordinator.recordInteractiveRealtimeSample(sample);
        }

        void recordRealtimeRejectedAttempt()
        {
            m_coordinator.recordRealtimeRejectedAttempt();
        }

        [[nodiscard]] bool isRealtimeSchedulingActive() const noexcept
        {
            return m_coordinator.isRealtimeSchedulingActive();
        }

        [[nodiscard]] bool isRealtimeActive() const noexcept
        {
            return m_coordinator.isRealtimeActive();
        }

        [[nodiscard]] bool isAutoPreviewFallbackActive() const noexcept
        {
            return m_coordinator.isAutoPreviewFallbackActive();
        }

        [[nodiscard]] RenderStamp latestStamp() const noexcept
        {
            return m_coordinator.latestStamp();
        }

        [[nodiscard]] RenderInteractionState interactionState() const noexcept
        {
            return m_coordinator.interactionState();
        }

        [[nodiscard]] std::optional<PresentedFrame> const & presentedFrame() const noexcept
        {
            return m_presentedFrames.presentedFrame();
        }

        void seedPresentedFrame(PresentedFrame frame)
        {
            m_presentedFrames.seedPresentedFrame(frame);
        }

        void clearPresentedFrame() noexcept
        {
            m_presentedFrames.clearPresentedFrame();
        }

        [[nodiscard]] RenderWorkflowDecision configureViewport(uint32_t width, uint32_t height)
        {
            return applyCoordinatorDecision(m_coordinator.configureViewport(width, height));
        }

        [[nodiscard]] RenderWorkflowDecision notifyCameraChanged()
        {
            return applyCoordinatorDecision(m_coordinator.notifyCameraChanged());
        }

        [[nodiscard]] RenderWorkflowDecision notifyCameraInteractionEnded()
        {
            return applyCoordinatorDecision(m_coordinator.notifyCameraInteractionEnded());
        }

        [[nodiscard]] RenderWorkflowDecision notifyParameterChanged(bool interactionActive)
        {
            return applyCoordinatorDecision(m_coordinator.notifyParameterChanged(interactionActive));
        }

        [[nodiscard]] RenderWorkflowDecision notifyParameterInteractionEnded()
        {
            return applyCoordinatorDecision(m_coordinator.notifyParameterInteractionEnded());
        }

        [[nodiscard]] RenderWorkflowDecision notifyStructuralModelChanged()
        {
            return applyCoordinatorDecision(m_coordinator.notifyStructuralModelChanged());
        }

        [[nodiscard]] RenderWorkflowDecision notifyProgramCompilationCompleted()
        {
            return applyCoordinatorDecision(m_coordinator.notifyProgramCompilationCompleted());
        }

        [[nodiscard]] RenderWorkflowDecision tick()
        {
            return applyCoordinatorDecision(m_coordinator.tick());
        }

        [[nodiscard]] RenderWorkflowDecision completeTask(RenderTaskResult const & result)
        {
            return applyCoordinatorDecision(m_coordinator.completeTask(result));
        }

        [[nodiscard]] bool canPresentCandidate(
          FramePresentationCandidate const & candidate) const noexcept
        {
            return m_presentedFrames.canPresentCandidate(candidate, latestStamp());
        }

        [[nodiscard]] bool presentCandidate(FramePresentationCandidate const & candidate) noexcept
        {
            return m_presentedFrames.presentCandidate(candidate, latestStamp());
        }

        [[nodiscard]] bool presentCandidate(FramePresentationCandidate const & candidate,
                                            RenderStamp const & requiredStamp) noexcept
        {
            return m_presentedFrames.presentCandidate(candidate, requiredStamp);
        }

      private:
        [[nodiscard]] static constexpr bool isDisplayTask(RenderTaskType type) noexcept
        {
            return type == RenderTaskType::RealtimeFullFrame ||
                   type == RenderTaskType::StaticFullFrameProbe ||
                   type == RenderTaskType::ProgressiveHighQualityChunk ||
                   type == RenderTaskType::LowResolutionPreview ||
                   type == RenderTaskType::StreamingPreview;
        }

        [[nodiscard]] static constexpr FramePresentationSource sourceForTask(
          RenderTaskType type) noexcept
        {
            switch (type)
            {
            case RenderTaskType::RealtimeFullFrame:
                return FramePresentationSource::ExactRealtime;
            case RenderTaskType::StaticFullFrameProbe:
                return FramePresentationSource::ProgressiveHighQuality;
            case RenderTaskType::LowResolutionPreview:
                return FramePresentationSource::LowResolutionPreview;
            case RenderTaskType::StreamingPreview:
                return FramePresentationSource::StreamingPreview;
            case RenderTaskType::ProgressiveHighQualityChunk:
                return FramePresentationSource::ProgressiveHighQuality;
            case RenderTaskType::BoundingBoxUpdate:
            case RenderTaskType::SdfPrecomputation:
            case RenderTaskType::ParameterUpload:
            case RenderTaskType::ProgramCompilation:
            default:
                return FramePresentationSource::Unknown;
            }
        }

        [[nodiscard]] static constexpr FramePresentationQuality qualityForResult(
          RenderTaskResult const & result) noexcept
        {
            switch (result.type)
            {
            case RenderTaskType::RealtimeFullFrame:
            case RenderTaskType::StaticFullFrameProbe:
                return FramePresentationQuality::FullQuality;
            case RenderTaskType::ProgressiveHighQualityChunk:
                return result.completedFrame ? FramePresentationQuality::FullQuality
                                             : FramePresentationQuality::ProgressivePartial;
            case RenderTaskType::LowResolutionPreview:
            case RenderTaskType::StreamingPreview:
                return FramePresentationQuality::Preview;
            case RenderTaskType::BoundingBoxUpdate:
            case RenderTaskType::SdfPrecomputation:
            case RenderTaskType::ParameterUpload:
            case RenderTaskType::ProgramCompilation:
            default:
                return FramePresentationQuality::Unknown;
            }
        }

        [[nodiscard]] FramePresentationCandidate makeCandidate(
          RenderTaskResult const & result) noexcept
        {
            auto const frameId = result.requestId == 0 ? m_nextSyntheticFrameId++
                                                       : result.requestId;
            return FramePresentationCandidate{.frameId = frameId,
                                              .stamp = result.stamp,
                                              .quality = qualityForResult(result),
                                              .source = sourceForTask(result.type),
                                              .completedFrame = result.completedFrame};
        }

        [[nodiscard]] RenderWorkflowDecision applyCoordinatorDecision(
          RenderUpdateDecision coordinatorDecision)
        {
            RenderWorkflowDecision workflowDecision{};
            workflowDecision.commands = std::move(coordinatorDecision.commands);

            for (auto const & command : workflowDecision.commands)
            {
                if (command.type != RenderCommandType::PresentFrame ||
                    !isDisplayTask(command.result.type) ||
                    !command.result.producedDisplayFrame)
                {
                    continue;
                }

                auto candidate = makeCandidate(command.result);
                if (presentCandidate(candidate))
                {
                    workflowDecision.acceptedFrames.push_back(candidate);
                    workflowDecision.presentedFrameChanged = true;
                }
                else
                {
                    workflowDecision.rejectedFrames.push_back(candidate);
                }
            }

            return workflowDecision;
        }

        RenderUpdateCoordinator m_coordinator;
        PresentedFrameLedger m_presentedFrames;
        uint64_t m_nextSyntheticFrameId{1};
    };
}
