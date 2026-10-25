#include "ui/render/RenderModeUpdatePolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithForceParameterInteraction_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Force,
                                   .interactionState = RenderInteractionState::ParameterInteracting,
                                   .stateIsMoving = true,
                                   .forceLowResRender = true,
                                   .lowResFeedbackPending = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithForceCameraInteraction_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Force,
                                   .interactionState = RenderInteractionState::CameraInteracting,
                                   .stateIsMoving = true,
                                   .forceLowResRender = true,
                                   .lowResFeedbackPending = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithForceStaticMoving_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Force,
                                   .interactionState = RenderInteractionState::Static,
                                   .stateIsMoving = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithAutoRealtimeActive_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Auto,
                                   .interactionState = RenderInteractionState::CameraInteracting,
                                   .autoRealtimeActive = true,
                                   .stateIsMoving = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithAutoRealtimeActiveStaticMoving_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Auto,
                                   .interactionState = RenderInteractionState::Static,
                                   .autoRealtimeActive = true,
                                   .stateIsMoving = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithAutoPreviewParameterInteraction_IsAllowed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Auto,
                                   .interactionState = RenderInteractionState::ParameterInteracting,
                                   .autoRealtimeActive = false,
                                   .lowResFeedbackPending = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_TRUE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithAutoRealtimeActiveParameterInteraction_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Auto,
                                   .interactionState = RenderInteractionState::ParameterInteracting,
                                   .autoRealtimeActive = true,
                                   .lowResFeedbackPending = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithOffCameraInteraction_IsAllowed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Off,
                                   .interactionState = RenderInteractionState::CameraInteracting,
                                   .stateIsMoving = true,
                                   .exactRealtimeJobInFlight = false});

        EXPECT_TRUE(decision);
    }

    TEST(RenderModeUpdatePolicy, LegacyLowResPreview_WithExactRealtimeInFlight_IsSuppressed)
    {
        auto const decision = shouldUseLegacyLowResPreview(
          LegacyLowResPreviewInput{.mode = RealtimeRaymarchMode::Off,
                                   .interactionState = RenderInteractionState::ParameterInteracting,
                                   .lowResFeedbackPending = true,
                                   .exactRealtimeJobInFlight = true});

        EXPECT_FALSE(decision);
    }

    TEST(RenderModeUpdatePolicy, ExactRealtimeInteraction_WithAutoRealtimeActive_IsActive)
    {
        auto const active = isExactRealtimeInteractionActive(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Auto,
                                           .interactionState = RenderInteractionState::CameraInteracting,
                                           .autoRealtimeActive = true,
                                           .autoPreviewFallbackActive = false,
                                           .exactRealtimeJobInFlight = false});

        EXPECT_TRUE(active);
    }

    TEST(RenderModeUpdatePolicy, ExactRealtimeInteraction_WithAutoFallbackPreview_IsNotActive)
    {
        auto const active = isExactRealtimeInteractionActive(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Auto,
                                           .interactionState = RenderInteractionState::CameraInteracting,
                                           .autoRealtimeActive = true,
                                           .autoPreviewFallbackActive = true,
                                           .exactRealtimeJobInFlight = false});

        EXPECT_FALSE(active);
    }

    TEST(RenderModeUpdatePolicy, ExactRealtimeInteraction_WithForce_IsActive)
    {
        auto const active = isExactRealtimeInteractionActive(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Force,
                                           .interactionState = RenderInteractionState::CameraInteracting});

        EXPECT_TRUE(active);
    }

    TEST(RenderModeUpdatePolicy, ExactRealtimeInteraction_WithStaticState_IsNotActive)
    {
        auto const active = isExactRealtimeInteractionActive(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Force,
                                           .interactionState = RenderInteractionState::Static,
                                           .exactRealtimeJobInFlight = true});

        EXPECT_FALSE(active);
    }

    TEST(RenderModeUpdatePolicy, RealtimeFrameExecution_WithStaticAutoRealtimeActive_UsesAsyncWorker)
    {
        auto const path = chooseRealtimeFrameExecutionPath(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Auto,
                                           .interactionState = RenderInteractionState::Static,
                                           .autoRealtimeActive = true});

        EXPECT_EQ(path, RealtimeFrameExecutionPath::AsyncWorker);
    }

    TEST(RenderModeUpdatePolicy, RealtimeFrameExecution_WithAutoExactInteraction_UsesSynchronousUiThread)
    {
        auto const path = chooseRealtimeFrameExecutionPath(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Auto,
                                           .interactionState = RenderInteractionState::CameraInteracting,
                                           .autoRealtimeActive = true,
                                           .autoPreviewFallbackActive = false});

        EXPECT_EQ(path, RealtimeFrameExecutionPath::SynchronousUiThread);
    }

    TEST(RenderModeUpdatePolicy, RealtimeFrameExecution_WithAutoPreviewFallback_UsesAsyncWorker)
    {
        auto const path = chooseRealtimeFrameExecutionPath(
          RealtimeInteractionActivityInput{.mode = RealtimeRaymarchMode::Auto,
                                           .interactionState = RenderInteractionState::CameraInteracting,
                                           .autoRealtimeActive = true,
                                           .autoPreviewFallbackActive = true});

        EXPECT_EQ(path, RealtimeFrameExecutionPath::AsyncWorker);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithForce_InvalidatesWithoutStreaming)
    {
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Force,
                                       .streamingPreviewActive = false});

        EXPECT_TRUE(dispatch.invalidateInteraction);
        EXPECT_FALSE(dispatch.refreshStreamingInteraction);
        EXPECT_FALSE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithAutoFirstChange_StartsStreaming)
    {
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Auto,
                                       .streamingPreviewActive = false});

        EXPECT_TRUE(dispatch.invalidateInteraction);
        EXPECT_FALSE(dispatch.refreshStreamingInteraction);
        EXPECT_TRUE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithOffFirstChange_StartsStreaming)
    {
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Off,
                                       .streamingPreviewActive = false});

        EXPECT_TRUE(dispatch.invalidateInteraction);
        EXPECT_FALSE(dispatch.refreshStreamingInteraction);
        EXPECT_TRUE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithAutoStreamingActive_RefreshesInteraction)
    {
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Auto,
                                       .streamingPreviewActive = true});

        EXPECT_FALSE(dispatch.invalidateInteraction);
        EXPECT_TRUE(dispatch.refreshStreamingInteraction);
        EXPECT_FALSE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithOffStreamingActive_RefreshesInteraction)
    {
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Off,
                                       .streamingPreviewActive = true});

        EXPECT_FALSE(dispatch.invalidateInteraction);
        EXPECT_TRUE(dispatch.refreshStreamingInteraction);
        EXPECT_FALSE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithAutoRealtimeActiveNoStreaming_InvalidatesWithoutStreaming)
    {
        // When Auto mode has confirmed the GPU is fast enough, parameter changes should
        // take the direct realtime path rather than starting a streaming preview.
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Auto,
                                       .streamingPreviewActive = false,
                                       .autoRealtimeActive = true});

        EXPECT_TRUE(dispatch.invalidateInteraction);
        EXPECT_FALSE(dispatch.refreshStreamingInteraction);
        EXPECT_FALSE(dispatch.startStreamingPreview);
    }

    TEST(RenderModeUpdatePolicy, ParameterChangeDispatch_WithAutoRealtimeActiveStreamingActive_InvalidatesWithoutStreaming)
    {
        // Even when streaming preview was already active, switching to realtime should
        // not keep feeding it.
        auto const dispatch = classifyParameterChange(
          ParameterChangeDispatchInput{.mode = RealtimeRaymarchMode::Auto,
                                       .streamingPreviewActive = true,
                                       .autoRealtimeActive = true});

        EXPECT_TRUE(dispatch.invalidateInteraction);
        EXPECT_FALSE(dispatch.refreshStreamingInteraction);
        EXPECT_FALSE(dispatch.startStreamingPreview);
    }
}
