#include "ui/render/PresentedFrameLedger.h"

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

        [[nodiscard]] constexpr PresentedFrame presentedFrame(
          RenderStamp stamp,
          FramePresentationQuality quality,
          FramePresentationSource source = FramePresentationSource::HeldFrame) noexcept
        {
            return PresentedFrame{.frameId = 1,
                                  .stamp = stamp,
                                  .quality = quality,
                                  .source = source,
                                  .completedFrame = true};
        }

        [[nodiscard]] constexpr FramePresentationCandidate candidate(
          RenderStamp stamp,
          FramePresentationQuality quality,
          FramePresentationSource source = FramePresentationSource::LowResolutionPreview,
          uint64_t frameId = 2) noexcept
        {
            return FramePresentationCandidate{.frameId = frameId,
                                              .stamp = stamp,
                                              .quality = quality,
                                              .source = source,
                                              .completedFrame = true};
        }
    }

    TEST(PresentedFrameLedger, CurrentPreview_WithNoPresentedFrame_IsAccepted)
    {
        PresentedFrameLedger ledger;
        auto const stamp = makeStamp(1, 2, 3, 4, 5);

        EXPECT_TRUE(ledger.presentCandidate(
          candidate(stamp, FramePresentationQuality::Preview), stamp));

        ASSERT_TRUE(ledger.presentedFrame().has_value());
        EXPECT_EQ(ledger.presentedFrame()->quality, FramePresentationQuality::Preview);
        EXPECT_EQ(ledger.presentedFrame()->source,
                  FramePresentationSource::LowResolutionPreview);
    }

    TEST(PresentedFrameLedger, StaleCandidate_IsRejected)
    {
        PresentedFrameLedger ledger;
        auto const oldStamp = makeStamp(1, 2, 3, 4, 5);
        auto const latestStamp = makeStamp(1, 2, 4, 4, 5);

        EXPECT_FALSE(ledger.presentCandidate(
          candidate(oldStamp, FramePresentationQuality::Preview), latestStamp));
        EXPECT_FALSE(ledger.presentedFrame().has_value());
    }

    TEST(PresentedFrameLedger, CurrentFullQuality_RejectsPreviewRegression)
    {
        PresentedFrameLedger ledger;
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        ledger.seedPresentedFrame(
          presentedFrame(stamp, FramePresentationQuality::FullQuality));

        EXPECT_FALSE(ledger.presentCandidate(
          candidate(stamp, FramePresentationQuality::Preview), stamp));

        ASSERT_TRUE(ledger.presentedFrame().has_value());
        EXPECT_EQ(ledger.presentedFrame()->quality, FramePresentationQuality::FullQuality);
    }

    TEST(PresentedFrameLedger, StaleFullQuality_AllowsFreshPreview)
    {
        PresentedFrameLedger ledger;
        auto const oldStamp = makeStamp(1, 2, 3, 4, 5);
        auto const latestStamp = makeStamp(1, 2, 4, 4, 5);
        ledger.seedPresentedFrame(
          presentedFrame(oldStamp, FramePresentationQuality::FullQuality));

        EXPECT_TRUE(ledger.presentCandidate(
          candidate(latestStamp, FramePresentationQuality::Preview), latestStamp));

        ASSERT_TRUE(ledger.presentedFrame().has_value());
        EXPECT_EQ(ledger.presentedFrame()->quality, FramePresentationQuality::Preview);
        EXPECT_TRUE(matches(ledger.presentedFrame()->stamp,
                            latestStamp,
                            RenderStampMask::displayFrame()));
    }

    TEST(PresentedFrameLedger, CurrentPreview_AllowsFreshFullQualityUpgrade)
    {
        PresentedFrameLedger ledger;
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        ledger.seedPresentedFrame(
          presentedFrame(stamp, FramePresentationQuality::Preview));

        EXPECT_TRUE(ledger.presentCandidate(
          candidate(stamp,
                    FramePresentationQuality::FullQuality,
                    FramePresentationSource::ProgressiveHighQuality),
          stamp));

        ASSERT_TRUE(ledger.presentedFrame().has_value());
        EXPECT_EQ(ledger.presentedFrame()->quality, FramePresentationQuality::FullQuality);
        EXPECT_EQ(ledger.presentedFrame()->source,
                  FramePresentationSource::ProgressiveHighQuality);
    }
}
