#pragma once

#include "RealtimeRaymarchController.h"
#include "RenderUpdateTypes.h"

namespace gladius::ui::async_rendering
{
    struct LegacyLowResPreviewInput
    {
        RealtimeRaymarchMode mode{RealtimeRaymarchMode::Auto};
        RenderInteractionState interactionState{RenderInteractionState::Static};
        bool autoRealtimeActive{false};
        bool stateIsMoving{false};
        bool forceLowResRender{false};
        bool lowResFeedbackPending{false};
        bool exactRealtimeJobInFlight{false};
    };

    [[nodiscard]] constexpr bool usesExactRealtimeInteraction(
      LegacyLowResPreviewInput const & input) noexcept
    {
        if (input.mode == RealtimeRaymarchMode::Force)
        {
            return true;
        }

        // Auto mode: once the learning controller has proven the GPU is fast enough,
        // suppress the legacy low-res path entirely.  The coordinator either schedules
        // exact realtime directly or keeps the current HQ frame while guards block.
        return input.mode == RealtimeRaymarchMode::Auto && input.autoRealtimeActive;
    }

    [[nodiscard]] constexpr bool shouldUseLegacyLowResPreview(
      LegacyLowResPreviewInput const & input) noexcept
    {
        bool const lowResRequested = input.stateIsMoving || input.forceLowResRender ||
                                     input.lowResFeedbackPending;
        if (!lowResRequested || input.exactRealtimeJobInFlight)
        {
            return false;
        }

        if (usesExactRealtimeInteraction(input))
        {
            return false;
        }

        return true;
    }

    struct RealtimeInteractionActivityInput
    {
        RealtimeRaymarchMode mode{RealtimeRaymarchMode::Auto};
        RenderInteractionState interactionState{RenderInteractionState::Static};
        bool autoRealtimeActive{false};
        /// True when Auto has fallen back to the simpler low-res preview path for the
        /// current gesture.  While this latch is active, preview tasks/results are the
        /// intended feedback path and must not be cancelled as "realtime active".
        bool autoPreviewFallbackActive{false};
        bool exactRealtimeJobInFlight{false};
    };

    [[nodiscard]] constexpr bool isExactRealtimeInteractionActive(
      RealtimeInteractionActivityInput const & input) noexcept
    {
        if (input.interactionState == RenderInteractionState::Static)
        {
            return false;
        }

        if (input.exactRealtimeJobInFlight)
        {
            return true;
        }

        if (input.mode == RealtimeRaymarchMode::Force)
        {
            return true;
        }

        return input.mode == RealtimeRaymarchMode::Auto && input.autoRealtimeActive &&
               !input.autoPreviewFallbackActive;
    }

    struct ParameterChangeDispatchInput
    {
        RealtimeRaymarchMode mode{RealtimeRaymarchMode::Auto};
        bool streamingPreviewActive{false};
        /// Set to true when the realtime learning controller has confirmed the GPU can
        /// render within the frame budget.  When true in Auto mode, parameter changes
        /// should use the direct realtime path rather than starting a streaming preview.
        bool autoRealtimeActive{false};
    };

    struct ParameterChangeDispatch
    {
        bool invalidateInteraction{false};
        bool refreshStreamingInteraction{false};
        bool startStreamingPreview{false};
    };

    [[nodiscard]] constexpr ParameterChangeDispatch classifyParameterChange(
      ParameterChangeDispatchInput const & input) noexcept
    {
        if (input.mode == RealtimeRaymarchMode::Force)
        {
            return {.invalidateInteraction = true,
                    .refreshStreamingInteraction = false,
                    .startStreamingPreview = false};
        }

        // Auto mode with realtime proven fast: treat like Force — just invalidate so the
        // coordinator can schedule a realtime frame.  Starting a streaming preview would
        // race with the sync realtime path and produce redundant low-res frames.
        if (input.mode == RealtimeRaymarchMode::Auto && input.autoRealtimeActive)
        {
            return {.invalidateInteraction = true,
                    .refreshStreamingInteraction = false,
                    .startStreamingPreview = false};
        }

        if (input.streamingPreviewActive)
        {
            return {.invalidateInteraction = false,
                    .refreshStreamingInteraction = true,
                    .startStreamingPreview = false};
        }

        return {.invalidateInteraction = true,
                .refreshStreamingInteraction = false,
                .startStreamingPreview = true};
    }
}