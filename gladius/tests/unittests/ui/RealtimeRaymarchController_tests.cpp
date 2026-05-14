#include "ui/render/RealtimeRaymarchController.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] RealtimeRaymarchConfig makeTestConfig()
        {
            RealtimeRaymarchConfig config{};
            config.targetFrameTimeMs = 25.0f;
            config.requiredFastSamples = 3;
            config.maxSlowSamples = 2;
            config.cooldownFrames = 4;
            config.ewmaAlpha = 1.0f;
            return config;
        }

        [[nodiscard]] RealtimeRaymarchSample makeFullFrameSample(float durationMs)
        {
            return RealtimeRaymarchSample{.durationMs = durationMs,
                                          .width = 800,
                                          .height = 600,
                                          .renderedLines = 600,
                                          .totalLines = 600,
                                          .completedFrame = true,
                                          .cancelled = false};
        }

        [[nodiscard]] RealtimeRaymarchGuards makeOpenGuards()
        {
            return RealtimeRaymarchGuards{};
        }
    }

    TEST(RealtimeRaymarchController, RecordFastSamples_EnablesRealtime)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordSample(makeFullFrameSample(18.0f));
        controller.recordSample(makeFullFrameSample(19.0f));
        controller.recordSample(makeFullFrameSample(20.0f));

        EXPECT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 20.0f);
    }

    TEST(RealtimeRaymarchController, RecordSlowSamples_EntersCooldown)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordSample(makeFullFrameSample(18.0f));
        controller.recordSample(makeFullFrameSample(18.0f));
        controller.recordSample(makeFullFrameSample(18.0f));
        ASSERT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));

        controller.recordSample(makeFullFrameSample(35.0f));
        controller.recordSample(makeFullFrameSample(36.0f));

        EXPECT_FALSE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        EXPECT_TRUE(controller.isCoolingDown());
    }

    TEST(RealtimeRaymarchController, BeginFrame_CountsDownCooldown)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordSample(makeFullFrameSample(60.0f));
        ASSERT_TRUE(controller.isCoolingDown());

        controller.beginFrame();
        controller.beginFrame();
        controller.beginFrame();
        EXPECT_TRUE(controller.isCoolingDown());

        controller.beginFrame();
        EXPECT_FALSE(controller.isCoolingDown());
    }

    TEST(RealtimeRaymarchController, RecordChunkSample_EstimatesFullFrameTime)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        RealtimeRaymarchSample sample{};
        sample.durationMs = 2.0f;
        sample.width = 800;
        sample.height = 600;
        sample.renderedLines = 60;
        sample.totalLines = 600;
        sample.completedFrame = false;

        controller.recordSample(sample);

        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 20.0f);
    }

    TEST(RealtimeRaymarchController, ForceMode_IgnoresLearningButNotGuards)
    {
        RealtimeRaymarchController controller;
        auto config = makeTestConfig();
        config.mode = RealtimeRaymarchMode::Force;
        controller.configure(config);
        controller.resetForResolution(800, 600);

        EXPECT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));

        auto blockedGuards = makeOpenGuards();
        blockedGuards.renderJobInFlight = true;
        EXPECT_FALSE(controller.canAttemptRealtime(800, 600, blockedGuards));
    }

    TEST(RealtimeRaymarchController, ResolutionChange_ResetsConfidence)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordSample(makeFullFrameSample(18.0f));
        controller.recordSample(makeFullFrameSample(18.0f));
        controller.recordSample(makeFullFrameSample(18.0f));
        ASSERT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));

        controller.resetForResolution(1024, 768);

        EXPECT_FALSE(controller.canAttemptRealtime(1024, 768, makeOpenGuards()));
        EXPECT_FALSE(controller.estimatedFullFrameTimeMs().has_value());
    }

    TEST(RealtimeRaymarchModeFromString, WithKnownValues_ParsesModes)
    {
        EXPECT_EQ(realtimeRaymarchModeFromString("off"), RealtimeRaymarchMode::Off);
        EXPECT_EQ(realtimeRaymarchModeFromString("force"), RealtimeRaymarchMode::Force);
        EXPECT_EQ(realtimeRaymarchModeFromString("auto"), RealtimeRaymarchMode::Auto);
        EXPECT_EQ(realtimeRaymarchModeFromString("unknown"), RealtimeRaymarchMode::Auto);
    }
}
