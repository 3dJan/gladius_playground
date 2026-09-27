#include "ui/SliceContourPolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::tests
{
    TEST(SliceContourPolicy, MissingBounds_WithChangedParameters_StartsFallbackGeneration)
    {
        auto const decision = decideWebGpuContourRequest(
          WebGpuContourRequestInput{.parametersChanged = true,
                                    .generationInFlight = false,
                                    .cacheValid = false,
                                    .throttleElapsed = true,
                                    .modelBoundsAvailable = false});

        EXPECT_TRUE(decision.shouldGenerate);
        EXPECT_TRUE(decision.useBuildVolumeFallback);
    }

    TEST(SliceContourPolicy, MissingBounds_WithinThrottleInterval_DoesNotStartGeneration)
    {
        auto const decision = decideWebGpuContourRequest(
          WebGpuContourRequestInput{.parametersChanged = true,
                                    .generationInFlight = false,
                                    .cacheValid = false,
                                    .throttleElapsed = false,
                                    .modelBoundsAvailable = false});

        EXPECT_FALSE(decision.shouldGenerate);
        EXPECT_FALSE(decision.useBuildVolumeFallback);
    }

    TEST(SliceContourPolicy, GenerationInFlight_DoesNotStartDuplicate)
    {
        auto const decision = decideWebGpuContourRequest(
          WebGpuContourRequestInput{.parametersChanged = true,
                                    .generationInFlight = true,
                                    .cacheValid = true,
                                    .throttleElapsed = true,
                                    .modelBoundsAvailable = false});

        EXPECT_FALSE(decision.shouldGenerate);
    }

    TEST(SliceContourPolicy, AvailableModelBounds_DoNotUseBuildVolumeFallback)
    {
        auto const decision = decideWebGpuContourRequest(
          WebGpuContourRequestInput{.parametersChanged = true,
                                    .generationInFlight = false,
                                    .cacheValid = false,
                                    .throttleElapsed = true,
                                    .modelBoundsAvailable = true});

        EXPECT_TRUE(decision.shouldGenerate);
        EXPECT_FALSE(decision.useBuildVolumeFallback);
    }
}
