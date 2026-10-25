#include "ui/render/PreviewResultAcceptancePolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] constexpr RenderStamp makeStamp(uint64_t scene,
                                                      uint64_t parameters,
                                                      uint64_t view,
                                                      uint64_t viewport,
                                                      uint64_t quality) noexcept
        {
            return RenderStamp{.sceneEpoch = scene,
                               .parameterEpoch = parameters,
                               .viewEpoch = view,
                               .viewportEpoch = viewport,
                               .qualityEpoch = quality};
        }

        [[nodiscard]] constexpr PreviewResultMeta previewResult(RenderStamp stamp) noexcept
        {
            return PreviewResultMeta{.frameId = 2,
                                     .epoch = 3,
                                     .viewEpoch = 4,
                                     .quality = FramePresentationQuality::Preview,
                                     .coordinatorStamp = stamp};
        }

        [[nodiscard]] constexpr PresentedFrame fullQualityFrame(RenderStamp stamp) noexcept
        {
            return PresentedFrame{.frameId = 1,
                                  .stamp = stamp,
                                  .quality = FramePresentationQuality::FullQuality,
                                  .source = FramePresentationSource::ProgressiveHighQuality,
                                  .completedFrame = true};
        }
    }

    TEST(PreviewResultAcceptancePolicy, CurrentPreview_WithNoPresentedFrame_IsAccepted)
    {
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const meta = previewResult(stamp);

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = stamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch});

        EXPECT_TRUE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::None);
    }

    TEST(PreviewResultAcceptancePolicy, StaleViewPreview_IsRejectedBeforePublish)
    {
        auto const oldStamp = makeStamp(1, 2, 3, 4, 5);
        auto const latestStamp = makeStamp(1, 2, 4, 4, 5);
        auto meta = previewResult(oldStamp);
        meta.viewEpoch = 3;

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = latestStamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = 4});

        EXPECT_FALSE(decision.accepted());
        EXPECT_TRUE(decision.shouldCompleteAsCancelled());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::StaleFrame);
    }

    TEST(PreviewResultAcceptancePolicy, CameraInteraction_WithStaleViewPreview_AllowsPresentationAsLatestView)
    {
        auto const oldViewStamp = makeStamp(1, 2, 3, 4, 5);
        auto const latestStamp = makeStamp(1, 2, 4, 4, 5);
        auto meta = previewResult(oldViewStamp);
        meta.viewEpoch = 3;

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = latestStamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = 4,
                                         .allowStaleViewDuringCameraInteraction = true});

        EXPECT_TRUE(decision.accepted());
        EXPECT_TRUE(decision.presentAsRequiredStamp);
    }

    TEST(PreviewResultAcceptancePolicy, CameraInteraction_WithStaleParameterPreview_RemainsRejected)
    {
        auto const oldParameterStamp = makeStamp(1, 2, 3, 4, 5);
        auto const latestStamp = makeStamp(1, 3, 4, 4, 5);
        auto meta = previewResult(oldParameterStamp);
        meta.viewEpoch = 3;

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = latestStamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = 4,
                                         .allowStaleViewDuringCameraInteraction = true});

        EXPECT_FALSE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::StaleFrame);
    }

    TEST(PreviewResultAcceptancePolicy, ActiveRealtimeInteraction_RejectsPreview)
    {
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const meta = previewResult(stamp);

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = stamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch,
                                         .realtimeRaymarchInteractionActive = true});

        EXPECT_FALSE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason,
                  PreviewResultRejectionReason::RealtimeRaymarchActive);
    }

    TEST(PreviewResultAcceptancePolicy, CurrentFullQualityFrame_RejectsPreviewRegression)
    {
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const meta = previewResult(stamp);

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = stamp,
                                         .presentedFrame = fullQualityFrame(stamp),
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch});

        EXPECT_FALSE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::QualityRegression);
    }

    TEST(PreviewResultAcceptancePolicy, StaleFullQualityFrame_AllowsFreshPreview)
    {
        auto const oldStamp = makeStamp(1, 2, 2, 4, 5);
        auto const latestStamp = makeStamp(1, 2, 3, 4, 5);
        auto const meta = previewResult(latestStamp);

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = latestStamp,
                                         .presentedFrame = fullQualityFrame(oldStamp),
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch});

        EXPECT_TRUE(decision.accepted());
    }

    TEST(PreviewResultAcceptancePolicy, CancelledPreview_IsRejected)
    {
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto meta = previewResult(stamp);
        meta.cancelled = true;

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = stamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch});

        EXPECT_FALSE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::Cancelled);
    }

    TEST(PreviewResultAcceptancePolicy, OlderPreviewFrameId_IsRejected)
    {
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const meta = previewResult(stamp);

        auto const decision = evaluatePreviewResultAcceptance(
          meta,
          PreviewResultAcceptanceContext{.requiredStamp = stamp,
                                         .currentEpoch = meta.epoch,
                                         .currentViewEpoch = meta.viewEpoch,
                                         .lastPresentedPreviewFrameId = meta.frameId});

        EXPECT_FALSE(decision.accepted());
        EXPECT_EQ(decision.rejectionReason, PreviewResultRejectionReason::OutOfOrderFrame);
    }
}
