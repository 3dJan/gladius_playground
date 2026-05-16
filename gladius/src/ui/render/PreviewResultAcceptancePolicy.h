#pragma once

#include "AsyncRenderTypes.h"
#include "FramePresentationTypes.h"

#include <optional>

namespace gladius::ui::async_rendering
{
    enum class PreviewResultRejectionReason
    {
        None,
        Cancelled,
        RealtimeRaymarchActive,
        StaleFrame,
        OutOfOrderFrame,
        QualityRegression
    };

    struct PreviewResultAcceptanceContext
    {
        RenderStamp requiredStamp{};
        std::optional<PresentedFrame> presentedFrame{};
        uint64_t currentEpoch{0};
        uint64_t currentViewEpoch{0};
        uint64_t lastPresentedPreviewFrameId{0};
        bool realtimeRaymarchInteractionActive{false};
        RenderStampMask freshnessMask{RenderStampMask::displayFrame()};
    };

    struct PreviewResultAcceptanceDecision
    {
        PreviewResultRejectionReason rejectionReason{PreviewResultRejectionReason::None};

        [[nodiscard]] constexpr bool accepted() const noexcept
        {
            return rejectionReason == PreviewResultRejectionReason::None;
        }

        [[nodiscard]] constexpr bool shouldCompleteAsCancelled() const noexcept
        {
            return !accepted();
        }
    };

    [[nodiscard]] constexpr char const * previewResultRejectionReasonName(
      PreviewResultRejectionReason reason) noexcept
    {
        switch (reason)
        {
        case PreviewResultRejectionReason::None:
            return "none";
        case PreviewResultRejectionReason::Cancelled:
            return "cancelled";
        case PreviewResultRejectionReason::RealtimeRaymarchActive:
            return "realtime raymarch active";
        case PreviewResultRejectionReason::StaleFrame:
            return "stale frame";
        case PreviewResultRejectionReason::OutOfOrderFrame:
            return "out-of-order frame";
        case PreviewResultRejectionReason::QualityRegression:
            return "quality regression";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] constexpr bool isPreviewResultStale(
      PreviewResultMeta const & meta,
      PreviewResultAcceptanceContext const & context) noexcept
    {
        bool const staleEpoch = meta.epoch < context.currentEpoch;
        bool const staleViewEpoch = meta.viewEpoch != 0u && meta.viewEpoch < context.currentViewEpoch;
        bool const staleStamp = !matches(meta.coordinatorStamp,
                                         context.requiredStamp,
                                         context.freshnessMask);
        return staleEpoch || staleViewEpoch || staleStamp;
    }

    [[nodiscard]] constexpr bool wouldRegressPresentedQuality(
      PreviewResultMeta const & meta,
      PreviewResultAcceptanceContext const & context) noexcept
    {
        if (!context.presentedFrame.has_value())
        {
            return false;
        }

        auto const & current = *context.presentedFrame;
        if (!matches(current.stamp, context.requiredStamp, context.freshnessMask))
        {
            return false;
        }

        return framePresentationQualityRank(meta.quality) <
               framePresentationQualityRank(current.quality);
    }

    /**
     * @brief Decides whether a completed low-resolution/streaming preview may be published.
     *
     * The decision is intentionally metadata-only so production can reject stale previews before
     * resampling into the shared result image, and tests can cover user-visible presentation rules
     * without OpenGL/OpenCL resources.
     */
    [[nodiscard]] constexpr PreviewResultAcceptanceDecision evaluatePreviewResultAcceptance(
      PreviewResultMeta const & meta,
      PreviewResultAcceptanceContext const & context) noexcept
    {
        if (meta.cancelled)
        {
            return {PreviewResultRejectionReason::Cancelled};
        }

        if (context.realtimeRaymarchInteractionActive)
        {
            return {PreviewResultRejectionReason::RealtimeRaymarchActive};
        }

        if (isPreviewResultStale(meta, context))
        {
            return {PreviewResultRejectionReason::StaleFrame};
        }

        if (meta.frameId <= context.lastPresentedPreviewFrameId)
        {
            return {PreviewResultRejectionReason::OutOfOrderFrame};
        }

        if (wouldRegressPresentedQuality(meta, context))
        {
            return {PreviewResultRejectionReason::QualityRegression};
        }

        return {};
    }
}
