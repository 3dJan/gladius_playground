#pragma once

#include "RealtimeRaymarchController.h"
#include "RenderUpdateTypes.h"

namespace gladius::ui::async_rendering
{
    /// Pure policy decision for which interactive frame path should be used next.
    /// This preserves the current runtime behavior while moving the branchy mode/
    /// interaction logic out of RenderUpdateCoordinator so it can be tested in
    /// isolation before the larger RenderWindow refactor lands.
    enum class InteractiveRenderPath
    {
        KeepCurrentFrame,
        ExactRealtime,
        LowResolutionPreview
    };

    struct InteractiveRenderPathInput
    {
        RealtimeRaymarchMode mode{RealtimeRaymarchMode::Auto};
        RenderInteractionState interactionState{RenderInteractionState::Static};
        bool interactiveFrameAlreadyInFlight{false};
        /// Auto parameter drag still keys off the learning controller's active flag.
        bool autoParameterExactRealtimeActive{false};
        /// Auto camera/static interaction admits exact realtime once static HQ timing
        /// has shown the scene is fast enough at the current resolution.
        bool autoInteractiveExactRealtimeAdmitted{false};
        /// Per-gesture hysteresis that keeps Auto on the simpler preview path once it
        /// has fallen back during the current interaction.
        bool preferSimplerPreview{false};
        /// True when the renderer/guards allow an exact realtime frame right now.
        bool exactRealtimeAllowed{false};
    };

    [[nodiscard]] constexpr InteractiveRenderPath chooseInteractiveRenderPath(
      InteractiveRenderPathInput const & input) noexcept
    {
        if (input.interactiveFrameAlreadyInFlight)
        {
            return InteractiveRenderPath::KeepCurrentFrame;
        }

        if (input.interactionState == RenderInteractionState::ParameterInteracting)
        {
            if (input.mode == RealtimeRaymarchMode::Force)
            {
                return input.exactRealtimeAllowed ? InteractiveRenderPath::ExactRealtime
                                                  : InteractiveRenderPath::KeepCurrentFrame;
            }

            if (input.mode == RealtimeRaymarchMode::Auto &&
                input.autoParameterExactRealtimeActive)
            {
                return input.exactRealtimeAllowed ? InteractiveRenderPath::ExactRealtime
                                                  : InteractiveRenderPath::KeepCurrentFrame;
            }

            return InteractiveRenderPath::LowResolutionPreview;
        }

        if (input.mode == RealtimeRaymarchMode::Auto)
        {
            if (input.preferSimplerPreview || !input.autoInteractiveExactRealtimeAdmitted)
            {
                return InteractiveRenderPath::LowResolutionPreview;
            }

            return input.exactRealtimeAllowed ? InteractiveRenderPath::ExactRealtime
                                              : InteractiveRenderPath::LowResolutionPreview;
        }

        if (input.mode == RealtimeRaymarchMode::Off)
        {
            return InteractiveRenderPath::LowResolutionPreview;
        }

        return input.exactRealtimeAllowed ? InteractiveRenderPath::ExactRealtime
                                          : InteractiveRenderPath::KeepCurrentFrame;
    }
}
