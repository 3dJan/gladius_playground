#include "ui/render/DisplayFrameSelector.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] constexpr DisplayFrameBufferState buffer(uint64_t epoch,
                                                               uint64_t viewEpoch) noexcept
        {
            return DisplayFrameBufferState{.hasImage = true,
                                           .epoch = epoch,
                                           .viewEpoch = viewEpoch};
        }
    }

    TEST(DisplayFrameSelector, WithCurrentFrontBuffer_SelectsFrontBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithStaleViewFrontBufferOutsideRealtime_FallsBackToResultImage)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 6),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithAutoPreviewFallbackResultAndStaleFront_SelectsResultImage)
    {
        auto const presentedPreview = PresentedFrame{.frameId = 11,
                                                     .stamp = RenderStamp{.sceneEpoch = 3,
                                                                          .viewEpoch = 7},
                                                     .quality = FramePresentationQuality::Preview,
                                                     .source = FramePresentationSource::LowResolutionPreview,
                                                     .completedFrame = true};

        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 6),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .exactRealtimeInteraction = false,
                                     .resultImageAvailable = true,
                                     .presentedFrame = presentedPreview});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithPreviouslyPresentedRealtimeFrontAfterEpochBump_HoldsFrontBuffer)
    {
                auto const presentedFrame = PresentedFrame{.frameId = 10,
                                                                                                     .stamp = RenderStamp{.sceneEpoch = 3,
                                                                                                                                                .viewEpoch = 7},
                                                                                                     .quality = FramePresentationQuality::FullQuality,
                                                                                                     .source = FramePresentationSource::ExactRealtime,
                                                                                                     .completedFrame = true};
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 7),
                                     .currentEpoch = 4,
                                     .currentViewEpoch = 8,
                                                                         .resultImageAvailable = true,
                                                                         .presentedFrame = presentedFrame});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithStaleViewFrontBufferDuringRealtimeInteraction_SelectsFrontBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 6),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .exactRealtimeInteraction = true});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithStaleViewFrontBufferDuringRealtimeJob_SelectsFrontBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 6),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .exactRealtimeJobInFlight = true});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithRenderingInProgressOutsideRealtime_BlocksFrontBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .isRendering = true});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithRenderingInProgressDuringRealtime_AllowsFrontBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .exactRealtimeInteraction = true,
                                     .isRendering = true});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithNoUsableFrontAndCurrentProgressive_SelectsProgressiveBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(2, 7),
                                     .progressiveBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7});

        EXPECT_EQ(selected, DisplayFrameSource::ProgressiveBuffer);
    }

    TEST(DisplayFrameSelector, WithPresentedFullFrameAndCurrentProgressive_HoldsFrontBuffer)
    {
        auto const presentedFrame = PresentedFrame{.frameId = 10,
                                                   .stamp = RenderStamp{.sceneEpoch = 2,
                                                                        .viewEpoch = 6},
                                                   .quality = FramePresentationQuality::FullQuality,
                                                   .source = FramePresentationSource::ProgressiveHighQuality,
                                                   .completedFrame = true};

        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(2, 6),
                                     .progressiveBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .presentedFrame = presentedFrame});

        EXPECT_EQ(selected, DisplayFrameSource::FrontBuffer);
    }

    TEST(DisplayFrameSelector, WithFullFrameJobInFlight_DoesNotSelectProgressiveBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(2, 7),
                                     .progressiveBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .fullFrameRenderJobInFlight = true});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithMovingState_DoesNotSelectProgressiveBuffer)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.progressiveBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .isMoving = true});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithSuppressedHq_FallsBackToResultImage)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.frontBuffer = buffer(3, 7),
                                     .progressiveBuffer = buffer(3, 7),
                                     .currentEpoch = 3,
                                     .currentViewEpoch = 7,
                                     .suppressHqDisplay = true});

        EXPECT_EQ(selected, DisplayFrameSource::ResultImage);
    }

    TEST(DisplayFrameSelector, WithoutAnyImage_ReturnsNone)
    {
        auto const selected = selectDisplayFrameSource(
          DisplayFrameSelectionInput{.resultImageAvailable = false});

        EXPECT_EQ(selected, DisplayFrameSource::None);
    }
}
