#pragma once

#include <cstdint>

namespace gladius::ui::async_rendering
{
    enum class DisplayFrameSource
    {
        None,
        FrontBuffer,
        ProgressiveBuffer,
        ResultImage
    };

    struct DisplayFrameBufferState
    {
        bool hasImage{false};
        uint64_t epoch{0};
        uint64_t viewEpoch{0};
    };

    struct DisplayFrameSelectionInput
    {
        DisplayFrameBufferState frontBuffer{};
        DisplayFrameBufferState progressiveBuffer{};
        uint64_t currentEpoch{0};
        uint64_t currentViewEpoch{0};
        bool exactRealtimeInteraction{false};
        bool exactRealtimeJobInFlight{false};
        bool isRendering{false};
        bool isMoving{false};
        bool suppressHqDisplay{false};
        bool resultImageAvailable{true};
    };

    /**
     * @brief Chooses which already-produced image should be drawn in the preview window.
     *
     * This is intentionally metadata-only so the UI can be tested without GL/OpenCL.
     * The current behavior prefers a current HQ/front buffer, permits a current-epoch
     * stale-view front buffer during exact realtime interaction, then falls back to a
     * current progressive buffer, and finally to the result image to avoid blanking.
     */
    [[nodiscard]] constexpr DisplayFrameSource selectDisplayFrameSource(
      DisplayFrameSelectionInput const & input) noexcept
    {
        bool const frontEpochMatches = input.frontBuffer.hasImage &&
                                       input.frontBuffer.epoch == input.currentEpoch;
        bool const frontViewMatches = input.frontBuffer.hasImage &&
                                      input.frontBuffer.viewEpoch == input.currentViewEpoch;
        bool const allowRealtimeFront =
          (input.exactRealtimeInteraction || input.exactRealtimeJobInFlight) &&
          frontEpochMatches;
        bool const frontBlockedByRendering = input.isRendering && !allowRealtimeFront;

        if (input.frontBuffer.hasImage && frontEpochMatches &&
            (frontViewMatches || allowRealtimeFront) && !frontBlockedByRendering &&
            !input.suppressHqDisplay)
        {
            return DisplayFrameSource::FrontBuffer;
        }

        bool const progressiveBufferCurrent =
          input.progressiveBuffer.hasImage &&
          input.progressiveBuffer.epoch == input.currentEpoch &&
          input.progressiveBuffer.viewEpoch == input.currentViewEpoch;

        if (progressiveBufferCurrent && !input.isMoving && !input.suppressHqDisplay)
        {
            return DisplayFrameSource::ProgressiveBuffer;
        }

        return input.resultImageAvailable ? DisplayFrameSource::ResultImage
                                          : DisplayFrameSource::None;
    }
}
