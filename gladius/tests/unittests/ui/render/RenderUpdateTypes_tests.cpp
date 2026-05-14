#include "ui/render/RenderUpdateTypes.h"

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

        [[nodiscard]] constexpr RenderTaskResult makeDisplayResult(RenderStamp stamp) noexcept
        {
            return RenderTaskResult{.requestId = 42,
                                    .type = RenderTaskType::RealtimeFullFrame,
                                    .stamp = stamp,
                                    .status = RenderTaskStatus::Completed,
                                    .durationNs = 1'000'000,
                                    .producedDisplayFrame = true,
                                    .completedFrame = true};
        }
    }

    TEST(RenderStampMatches, WithSameRequiredAxes_ReturnsTrue)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const candidate = makeStamp(3, 5, 99, 101, 103);

        EXPECT_TRUE(matches(candidate, latest, RenderStampMask::sceneAndParameters()));
    }

    TEST(RenderStampMatches, WithDifferentView_FailsDisplayFrameButPassesHeavyGeometryTask)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const cameraOnlyStale = makeStamp(3, 5, 6, 11, 13);

        EXPECT_FALSE(matches(cameraOnlyStale, latest, RenderStampMask::displayFrame()));
        EXPECT_TRUE(matches(cameraOnlyStale, latest, RenderStampMask::heavyGeometryTask()));
    }

    TEST(RenderStampIsOlderThan, WithIgnoredAxis_DoesNotReportOlder)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const oldViewOnly = makeStamp(3, 5, 1, 11, 13);

        EXPECT_FALSE(isOlderThan(oldViewOnly, latest, RenderStampMask::heavyGeometryTask()));
        EXPECT_TRUE(isOlderThan(oldViewOnly, latest, RenderStampMask::displayFrame()));
    }

    TEST(RenderTaskResultIsDisplayableFor, WithCurrentDisplayStamp_ReturnsTrue)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const result = makeDisplayResult(latest);

        EXPECT_TRUE(result.isDisplayableFor(latest));
    }

    TEST(RenderTaskResultIsDisplayableFor, WithOldCameraStamp_ReturnsFalse)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const oldCamera = makeStamp(3, 5, 6, 11, 13);
        auto const result = makeDisplayResult(oldCamera);

        EXPECT_FALSE(result.isDisplayableFor(latest));
    }

    TEST(RenderTaskResultIsDisplayableFor, WithOldParameterStamp_ReturnsFalse)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const oldParameter = makeStamp(3, 4, 7, 11, 13);
        auto const result = makeDisplayResult(oldParameter);

        EXPECT_FALSE(result.isDisplayableFor(latest));
    }

    TEST(RenderTaskResultIsCurrentFor, WithBoundingBoxResult_IgnoresCameraAndViewport)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto const bboxStamp = makeStamp(3, 5, 1, 2, 3);
        auto const result = RenderTaskResult{.requestId = 7,
                                             .type = RenderTaskType::BoundingBoxUpdate,
                                             .stamp = bboxStamp,
                                             .status = RenderTaskStatus::Completed};

        EXPECT_TRUE(result.isCurrentFor(latest, RenderStampMask::heavyGeometryTask()));
        EXPECT_FALSE(result.isCurrentFor(latest, RenderStampMask::displayFrame()));
    }

    TEST(RenderTaskResultIsCurrentFor, WithCancelledResult_ReturnsFalse)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto result = makeDisplayResult(latest);
        result.status = RenderTaskStatus::Cancelled;

        EXPECT_FALSE(result.isCurrentFor(latest, RenderStampMask::displayFrame()));
        EXPECT_FALSE(result.isDisplayableFor(latest));
    }

    TEST(RenderTaskResultIsDisplayableFor, WithoutDisplayFrame_ReturnsFalse)
    {
        auto const latest = makeStamp(3, 5, 7, 11, 13);
        auto result = makeDisplayResult(latest);
        result.producedDisplayFrame = false;

        EXPECT_FALSE(result.isDisplayableFor(latest));
    }
}
