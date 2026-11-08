#include "ui/render/ParameterInteractionPolicy.h"
#include "ui/render/RenderWorkflowController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace gladius::ui::async_rendering::tests
{
    using namespace std::chrono_literals;

    TEST(ParameterInteractionPolicy, ConsumeSettlement_WithoutEdits_DoesNotSettle)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};

        EXPECT_FALSE(policy.consumeSettlement(now + 1s));
        EXPECT_FALSE(policy.hasPendingSettlement());
    }

    TEST(ParameterInteractionPolicy, ConsumeSettlement_QuietPeriodBoundary_SettlesExactlyOnce)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};
        policy.notifyParameterChanged(now);

        EXPECT_TRUE(policy.hasPendingSettlement());
        EXPECT_FALSE(policy.consumeSettlement(now + 199ms));
        EXPECT_TRUE(policy.consumeSettlement(now + 200ms));
        EXPECT_FALSE(policy.hasPendingSettlement());
        EXPECT_FALSE(policy.consumeSettlement(now + 201ms));
        EXPECT_FALSE(policy.consumeSettlement(now + 1s));
    }

    TEST(ParameterInteractionPolicy, NotifyParameterChanged_ContinuingEdits_RenewsQuietPeriod)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};
        policy.notifyParameterChanged(now);
        policy.notifyParameterChanged(now + 150ms);

        EXPECT_FALSE(policy.consumeSettlement(now + 200ms));
        EXPECT_FALSE(policy.consumeSettlement(now + 349ms));
        EXPECT_TRUE(policy.consumeSettlement(now + 350ms));
    }

    TEST(ParameterInteractionPolicy, ConsumeSettlement_DelayedFrame_SettlesWithoutPollingDeadline)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};
        policy.notifyParameterChanged(now);

        EXPECT_TRUE(policy.consumeSettlement(now + 5s));
        EXPECT_FALSE(policy.consumeSettlement(now + 6s));
    }

    TEST(ParameterInteractionPolicy, ConsumeSettlement_EarlierTimestamp_DoesNotSettle)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{} + 1s;
        policy.notifyParameterChanged(now);

        EXPECT_FALSE(policy.consumeSettlement(now - 1ms));
        EXPECT_TRUE(policy.hasPendingSettlement());
        EXPECT_TRUE(policy.consumeSettlement(now + 200ms));
    }

    TEST(ParameterInteractionPolicy, Reset_ExplicitEndOrSupersedingInteraction_DisarmsSettlement)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};
        policy.notifyParameterChanged(now);

        policy.reset();

        EXPECT_FALSE(policy.hasPendingSettlement());
        EXPECT_FALSE(policy.consumeSettlement(now + 1s));
        policy.notifyParameterChanged(now + 2s);
        EXPECT_FALSE(policy.consumeSettlement(now + 2199ms));
        EXPECT_TRUE(policy.consumeSettlement(now + 2200ms));
    }

    TEST(ParameterInteractionPolicy, NotifyParameterChanged_AfterSettlement_RearmsSettlement)
    {
        ParameterInteractionPolicy policy;
        auto const now = ParameterInteractionPolicy::TimePoint{};
        policy.notifyParameterChanged(now);
        ASSERT_TRUE(policy.consumeSettlement(now + 200ms));

        policy.notifyParameterChanged(now + 250ms);

        EXPECT_FALSE(policy.consumeSettlement(now + 449ms));
        EXPECT_TRUE(policy.consumeSettlement(now + 450ms));
    }

    TEST(ParameterInteractionPolicy, ConsumeSettlement_EmbeddedParameterEdits_ResumesStaticHighQuality)
    {
        for (auto const mode : {RealtimeRaymarchMode::Off,
                                RealtimeRaymarchMode::Auto,
                                RealtimeRaymarchMode::Force})
        {
            SCOPED_TRACE(static_cast<int>(mode));
            ParameterInteractionPolicy policy;
            RenderWorkflowController workflow;
            RealtimeRaymarchConfig config;
            config.mode = mode;
            workflow.configureRealtime(config);
            auto const now = ParameterInteractionPolicy::TimePoint{};
            (void) workflow.notifyEmbeddedParameterChanged(true);
            policy.notifyParameterChanged(now);
            (void) workflow.notifyEmbeddedParameterChanged(true);
            policy.notifyParameterChanged(now + 150ms);
            auto const latestStamp = workflow.latestStamp();

            ASSERT_FALSE(policy.consumeSettlement(now + 200ms));
            EXPECT_EQ(workflow.interactionState(), RenderInteractionState::ParameterInteracting);
            ASSERT_TRUE(policy.consumeSettlement(now + 350ms));
            auto decision = workflow.notifyParameterInteractionEnded();

            EXPECT_EQ(workflow.interactionState(), RenderInteractionState::Static);
            EXPECT_TRUE(matches(workflow.latestStamp(), latestStamp, RenderStampMask::displayFrame()));
            bool reachedHighQuality = false;
            bool presentedPreview = false;
            for (int step = 0; step < 8; ++step)
            {
                auto const task = std::find_if(
                  decision.commands.begin(), decision.commands.end(),
                  [](RenderCommand const & command)
                  { return command.type == RenderCommandType::StartTask; });
                ASSERT_NE(task, decision.commands.end());
                auto const request = task->task;
                EXPECT_NE(request.type, RenderTaskType::ParameterUpload);
                EXPECT_NE(request.type, RenderTaskType::StreamingPreview);
                EXPECT_TRUE(matches(request.stamp, latestStamp, RenderStampMask::displayFrame()));
                bool const isHighQuality =
                  request.type == RenderTaskType::StaticFullFrameProbe ||
                  request.type == RenderTaskType::ProgressiveHighQualityChunk;
                bool const isPreview = request.type == RenderTaskType::LowResolutionPreview;
                if (isPreview)
                {
                    EXPECT_FALSE(reachedHighQuality);
                    presentedPreview = true;
                }
                if (isHighQuality)
                {
                    EXPECT_TRUE(presentedPreview);
                }
                decision = workflow.completeTask(RenderTaskResult{
                  .requestId = request.requestId,
                  .type = request.type,
                  .stamp = request.stamp,
                  .status = RenderTaskStatus::Completed,
                  .producedDisplayFrame = isHighQuality || isPreview,
                  .completedFrame = isHighQuality || isPreview});
                if (isHighQuality)
                {
                    reachedHighQuality = true;
                    break;
                }
            }

            ASSERT_TRUE(reachedHighQuality);
            EXPECT_TRUE(workflow.isHighQualityFrameCurrent());
            ASSERT_TRUE(workflow.presentedFrame().has_value());
            EXPECT_EQ(workflow.presentedFrame()->quality, FramePresentationQuality::FullQuality);
            EXPECT_FALSE(policy.consumeSettlement(now + 1s));
        }
    }
}