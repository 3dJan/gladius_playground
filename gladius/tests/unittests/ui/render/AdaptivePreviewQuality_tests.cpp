#include "ui/render/AdaptivePreviewQuality.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(AdaptivePreviewQuality, SlowRender_ReducesLinearQualityByPixelAreaRatio)
    {
        EXPECT_FLOAT_EQ(adaptInteractivePreviewQuality(0.5f, 25.0f, 100.0f, 0.05f, 1.0f),
                        0.25f);
    }

    TEST(AdaptivePreviewQuality, ExtremelySlowRender_LimitsSingleStepToHalfQuality)
    {
        EXPECT_FLOAT_EQ(adaptInteractivePreviewQuality(0.8f, 25.0f, 1000.0f, 0.05f, 1.0f),
                        0.4f);
    }

    TEST(AdaptivePreviewQuality, FastRender_LimitsSingleStepRecovery)
    {
        EXPECT_NEAR(adaptInteractivePreviewQuality(0.5f, 25.0f, 1.0f, 0.05f, 1.0f),
                    0.6f,
                    1e-6f);
    }

    TEST(AdaptivePreviewQuality, SlowRender_RespectsMinimumQuality)
    {
        EXPECT_FLOAT_EQ(adaptInteractivePreviewQuality(0.06f, 25.0f, 1000.0f, 0.05f, 1.0f),
                        0.05f);
    }
} // namespace gladius::ui::async_rendering::tests
