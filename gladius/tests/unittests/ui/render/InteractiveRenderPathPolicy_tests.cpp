#include "ui/render/InteractiveRenderPathPolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(InteractiveRenderPathPolicy, WithInteractiveFrameAlreadyInFlight_KeepsCurrentFrame)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Force,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .interactiveFrameAlreadyInFlight = true,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::KeepCurrentFrame);
    }

    TEST(InteractiveRenderPathPolicy, ForceParameter_WithRealtimeAllowed_UsesExactRealtime)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Force,
                                     .interactionState = RenderInteractionState::ParameterInteracting,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::ExactRealtime);
    }

    TEST(InteractiveRenderPathPolicy, ForceParameter_WithRealtimeBlocked_KeepsCurrentFrame)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Force,
                                     .interactionState = RenderInteractionState::ParameterInteracting,
                                     .exactRealtimeAllowed = false});

        EXPECT_EQ(path, InteractiveRenderPath::KeepCurrentFrame);
    }

    TEST(InteractiveRenderPathPolicy, AutoParameter_WithRealtimeActiveAndAllowed_UsesExactRealtime)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Auto,
                                     .interactionState = RenderInteractionState::ParameterInteracting,
                                     .autoParameterExactRealtimeActive = true,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::ExactRealtime);
    }

    TEST(InteractiveRenderPathPolicy, AutoParameter_WithoutRealtimeActive_UsesLowResolutionPreview)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Auto,
                                     .interactionState = RenderInteractionState::ParameterInteracting,
                                     .autoParameterExactRealtimeActive = false,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::LowResolutionPreview);
    }

    TEST(InteractiveRenderPathPolicy, AutoCamera_WithGestureLocked_UsesLowResolutionPreview)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Auto,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .autoInteractiveExactRealtimeAdmitted = true,
                                     .preferSimplerPreview = true,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::LowResolutionPreview);
    }

    TEST(InteractiveRenderPathPolicy, AutoCamera_WithRealtimeAdmittedAndAllowed_UsesExactRealtime)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Auto,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .autoInteractiveExactRealtimeAdmitted = true,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::ExactRealtime);
    }

    TEST(InteractiveRenderPathPolicy, AutoCamera_WithRealtimeAdmittedButBlocked_KeepsCurrentFrame)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Auto,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .autoInteractiveExactRealtimeAdmitted = true,
                                     .exactRealtimeAllowed = false});

        EXPECT_EQ(path, InteractiveRenderPath::KeepCurrentFrame);
    }

    TEST(InteractiveRenderPathPolicy, OffInteraction_UsesLowResolutionPreview)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Off,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .exactRealtimeAllowed = true});

        EXPECT_EQ(path, InteractiveRenderPath::LowResolutionPreview);
    }

    TEST(InteractiveRenderPathPolicy, ForceCamera_WithRealtimeBlocked_KeepsCurrentFrame)
    {
        auto const path = chooseInteractiveRenderPath(
          InteractiveRenderPathInput{.mode = RealtimeRaymarchMode::Force,
                                     .interactionState = RenderInteractionState::CameraInteracting,
                                     .exactRealtimeAllowed = false});

        EXPECT_EQ(path, InteractiveRenderPath::KeepCurrentFrame);
    }
}
