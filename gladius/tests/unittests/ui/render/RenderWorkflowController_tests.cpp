#include "ui/render/RenderWorkflowController.h"

#include <gtest/gtest.h>

#include <optional>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] std::optional<RenderTaskRequest> findStartedTask(
          RenderWorkflowDecision const & decision,
          RenderTaskType type)
        {
            for (auto const & command : decision.commands)
            {
                if (command.type == RenderCommandType::StartTask && command.task.type == type)
                {
                    return command.task;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool hasStartedTask(RenderWorkflowDecision const & decision,
                                          RenderTaskType type)
        {
            return findStartedTask(decision, type).has_value();
        }

        [[nodiscard]] bool hasCommand(RenderWorkflowDecision const & decision,
                                      RenderCommandType type)
        {
            for (auto const & command : decision.commands)
            {
                if (command.type == type)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::optional<RenderTaskRequest> findFirstDisplayTask(
          RenderWorkflowDecision const & decision)
        {
            if (auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame))
            {
                return realtime;
            }
            if (auto progressive = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk))
            {
                return progressive;
            }
            if (auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview))
            {
                return preview;
            }
            if (auto streaming = findStartedTask(decision, RenderTaskType::StreamingPreview))
            {
                return streaming;
            }
            return std::nullopt;
        }

        [[nodiscard]] RenderTaskResult completed(RenderTaskRequest const & request,
                                                 bool producedDisplayFrame = false,
                                                 bool completedFrame = false,
                                                 uint64_t durationNs = 50'000'000)
        {
            return RenderTaskResult{.requestId = request.requestId,
                                    .type = request.type,
                                    .stamp = request.stamp,
                                    .status = RenderTaskStatus::Completed,
                                    .durationNs = durationNs,
                                    .producedDisplayFrame = producedDisplayFrame,
                                    .completedFrame = completedFrame};
        }

        [[nodiscard]] RealtimeRaymarchConfig modeConfig(RealtimeRaymarchMode mode)
        {
            RealtimeRaymarchConfig config{};
            config.mode = mode;
            return config;
        }

        [[nodiscard]] PresentedFrame heldFullQualityFrame(RenderStamp const & stamp)
        {
            return PresentedFrame{.frameId = 1,
                                  .stamp = stamp,
                                  .quality = FramePresentationQuality::FullQuality,
                                  .source = FramePresentationSource::HeldFrame,
                                  .completedFrame = true};
        }
    }

    TEST(RenderWorkflowController,
         ForceParameterDrag_WithExactAvailable_PresentsRealtimeAndNeverPreview)
    {
        RenderWorkflowController workflow;
        workflow.configureRealtime(modeConfig(RealtimeRaymarchMode::Force));
        (void) workflow.configureViewport(640, 480);
        workflow.seedPresentedFrame(heldFullQualityFrame(workflow.latestStamp()));

        auto decision = workflow.notifyParameterChanged(true);

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(realtime.has_value());
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::StreamingPreview));

        decision = workflow.completeTask(completed(*realtime, true, true));

        ASSERT_EQ(decision.acceptedFrames.size(), 1u);
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->source, FramePresentationSource::ExactRealtime);
        EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::FullQuality);
        EXPECT_TRUE(matches(workflow.presentedFrame()->stamp,
                            workflow.latestStamp(),
                            RenderStampMask::displayFrame()));
    }

    TEST(RenderWorkflowController,
         ForceParameterDrag_WithRealtimeBlocked_HoldsCurrentFrameWithoutPreview)
    {
        RenderWorkflowController workflow;
        workflow.configureRealtime(modeConfig(RealtimeRaymarchMode::Force));
        (void) workflow.configureViewport(640, 480);
        auto const initialFrame = heldFullQualityFrame(workflow.latestStamp());
        workflow.seedPresentedFrame(initialFrame);

        RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        workflow.setRealtimeGuards(guards);

        auto const decision = workflow.notifyParameterChanged(true);

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::StreamingPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->frameId, initialFrame.frameId);
        EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::FullQuality);
    }

    TEST(RenderWorkflowController,
         CameraChanged_DuringParameterPreview_PreemptsAndRejectsStaleParameterPreview)
    {
        RenderWorkflowController workflow;
        workflow.configureRealtime(modeConfig(RealtimeRaymarchMode::Off));
        (void) workflow.configureViewport(640, 480);
        workflow.seedPresentedFrame(heldFullQualityFrame(workflow.latestStamp()));

        auto decision = workflow.notifyParameterChanged(true);
        auto parameterPreview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(parameterPreview.has_value());

        decision = workflow.notifyCameraChanged();
        auto cameraPreview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(cameraPreview.has_value());
        EXPECT_EQ(workflow.interactionState(), RenderInteractionState::CameraInteracting);
        EXPECT_FALSE(matches(parameterPreview->stamp,
                             workflow.latestStamp(),
                             RenderStampMask::displayFrame()));
        EXPECT_TRUE(matches(cameraPreview->stamp,
                            workflow.latestStamp(),
                            RenderStampMask::displayFrame()));

        decision = workflow.completeTask(completed(*parameterPreview, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));
        EXPECT_TRUE(decision.acceptedFrames.empty());
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->source, FramePresentationSource::HeldFrame);

        decision = workflow.completeTask(completed(*cameraPreview, true, true));
        ASSERT_EQ(decision.acceptedFrames.size(), 1u);
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->source, FramePresentationSource::LowResolutionPreview);
        EXPECT_TRUE(matches(workflow.presentedFrame()->stamp,
                            workflow.latestStamp(),
                            RenderStampMask::displayFrame()));
    }

    TEST(RenderWorkflowController,
         Presentation_WithCurrentFullQuality_RejectsSameStampPreviewRegression)
    {
        RenderWorkflowController workflow;
        (void) workflow.configureViewport(640, 480);
        workflow.seedPresentedFrame(heldFullQualityFrame(workflow.latestStamp()));

        auto const preview = FramePresentationCandidate{
          .frameId = 2,
          .stamp = workflow.latestStamp(),
          .quality = FramePresentationQuality::Preview,
          .source = FramePresentationSource::LowResolutionPreview,
          .completedFrame = true};

        EXPECT_FALSE(workflow.presentCandidate(preview));
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::FullQuality);
        EXPECT_EQ(workflow.presentedFrame()->source, FramePresentationSource::HeldFrame);
    }

    TEST(RenderWorkflowController,
         Presentation_WithStaleFullQuality_AllowsFreshPreview)
    {
        RenderWorkflowController workflow;
        workflow.configureRealtime(modeConfig(RealtimeRaymarchMode::Off));
        (void) workflow.configureViewport(640, 480);
        workflow.seedPresentedFrame(heldFullQualityFrame(workflow.latestStamp()));

        auto decision = workflow.notifyCameraChanged();
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());

        decision = workflow.completeTask(completed(*preview, true, true));

        ASSERT_EQ(decision.acceptedFrames.size(), 1u);
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::Preview);
        EXPECT_EQ(workflow.presentedFrame()->source, FramePresentationSource::LowResolutionPreview);
        EXPECT_TRUE(matches(workflow.presentedFrame()->stamp,
                            workflow.latestStamp(),
                            RenderStampMask::displayFrame()));
    }

    TEST(RenderWorkflowController,
         ParameterInteractionEnded_SequencesUploadBboxSdfThenHighQualityCatchUp)
    {
        RenderWorkflowController workflow;
        workflow.seedPresentedFrame(heldFullQualityFrame(workflow.latestStamp()));

        auto decision = workflow.notifyParameterChanged(true);
        auto upload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(upload.has_value());

        decision = workflow.completeTask(completed(*upload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));

        decision = workflow.notifyParameterInteractionEnded();
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = workflow.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());

        decision = workflow.completeTask(completed(*sdf));
        auto hq = findFirstDisplayTask(decision);
        ASSERT_TRUE(hq.has_value());
        EXPECT_NE(hq->type, RenderTaskType::LowResolutionPreview);
        EXPECT_NE(hq->type, RenderTaskType::StreamingPreview);

        decision = workflow.completeTask(completed(*hq, true, true));

        ASSERT_EQ(decision.acceptedFrames.size(), 1u);
        ASSERT_TRUE(workflow.presentedFrame().has_value());
        EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::FullQuality);
        EXPECT_TRUE(matches(workflow.presentedFrame()->stamp,
                            workflow.latestStamp(),
                            RenderStampMask::displayFrame()));
    }
}
