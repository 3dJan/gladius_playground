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

        [[nodiscard]] RealtimeRaymarchSample makeProgressiveChunkSample(float durationMs)
        {
            return RealtimeRaymarchSample{.durationMs = durationMs,
                                          .width = 800,
                                          .height = 600,
                                          .renderedLines = 60,
                                          .totalLines = 600,
                                          .completedFrame = false,
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

        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(19.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(20.0f));

        EXPECT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 20.0f);
    }

    TEST(RealtimeRaymarchController, InteractiveSlowSample_DeactivatesRealtime)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        ASSERT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));

        controller.recordInteractiveRealtimeSample(makeFullFrameSample(65.0f));

        EXPECT_FALSE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        EXPECT_FALSE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));
    }

    TEST(RealtimeRaymarchController, BeginFrame_CountsDownCooldown)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordRejectedAttempt();
        controller.recordRejectedAttempt();
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

        controller.recordStaticProgressiveSample(sample);

        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 20.0f);
        EXPECT_TRUE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));
    }

    TEST(RealtimeRaymarchController, AutoLearning_CanAttemptStaticFullFrameAfterFastProgressiveSample)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());
        controller.resetForResolution(800, 600);

        EXPECT_FALSE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        EXPECT_FALSE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));

        controller.recordStaticProgressiveSample(makeProgressiveChunkSample(2.0f));

        EXPECT_TRUE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));

        auto blockedGuards = makeOpenGuards();
        blockedGuards.renderJobInFlight = true;
        EXPECT_FALSE(controller.canAttemptStaticFullFrame(800, 600, blockedGuards));
    }

    TEST(RealtimeRaymarchController, SlowProgressiveSample_DoesNotEnableStaticFullFrame)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordStaticProgressiveSample(makeProgressiveChunkSample(10.0f));

        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 100.0f);
        EXPECT_FALSE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));
    }

    TEST(RealtimeRaymarchController, SlowProgressiveSample_ClearsStaticFullFramePreference)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordStaticProgressiveSample(makeProgressiveChunkSample(2.0f));
        ASSERT_TRUE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));

        controller.recordStaticProgressiveSample(makeProgressiveChunkSample(10.0f));

        EXPECT_FALSE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));
    }

    TEST(RealtimeRaymarchController, StaticFullFrameBetweenRealtimeAndStaticBudgets_DoesNotActivateRealtime)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordStaticProgressiveSample(makeProgressiveChunkSample(2.0f));
        ASSERT_TRUE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));

        controller.recordStaticFullFrameSample(makeFullFrameSample(40.0f));

        EXPECT_FALSE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));
        EXPECT_TRUE(controller.canAttemptStaticFullFrame(800, 600, makeOpenGuards()));
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

    TEST(RealtimeRaymarchController, ResolutionChange_ScalesConfidence)
    {
        RealtimeRaymarchController controller;
        controller.configure(makeTestConfig());

        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        controller.recordStaticFullFrameSample(makeFullFrameSample(18.0f));
        ASSERT_TRUE(controller.canAttemptRealtime(800, 600, makeOpenGuards()));

        controller.resetForResolution(400, 300);

        EXPECT_TRUE(controller.canAttemptRealtime(400, 300, makeOpenGuards()));
        ASSERT_TRUE(controller.estimatedFullFrameTimeMs().has_value());
        EXPECT_FLOAT_EQ(*controller.estimatedFullFrameTimeMs(), 4.5f);
    }

    TEST(RealtimeRaymarchModeFromString, WithKnownValues_ParsesModes)
    {
        EXPECT_EQ(realtimeRaymarchModeFromString("off"), RealtimeRaymarchMode::Off);
        EXPECT_EQ(realtimeRaymarchModeFromString("force"), RealtimeRaymarchMode::Force);
        EXPECT_EQ(realtimeRaymarchModeFromString("auto"), RealtimeRaymarchMode::Auto);
        EXPECT_EQ(realtimeRaymarchModeFromString("unknown"), RealtimeRaymarchMode::Auto);
    }
}
