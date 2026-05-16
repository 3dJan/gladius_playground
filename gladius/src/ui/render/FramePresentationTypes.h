#pragma once

#include "RenderUpdateTypes.h"

#include <cstdint>

namespace gladius::ui::async_rendering
{
    enum class FramePresentationQuality
    {
        Unknown,
        Preview,
        ProgressivePartial,
        FullQuality
    };

    [[nodiscard]] constexpr int framePresentationQualityRank(
      FramePresentationQuality quality) noexcept
    {
        switch (quality)
        {
        case FramePresentationQuality::Preview:
            return 1;
        case FramePresentationQuality::ProgressivePartial:
            return 2;
        case FramePresentationQuality::FullQuality:
            return 3;
        case FramePresentationQuality::Unknown:
        default:
            return 0;
        }
    }

    struct FramePresentationCandidate
    {
        uint64_t frameId{0};
        RenderStamp stamp{};
        FramePresentationQuality quality{FramePresentationQuality::Unknown};
    };
}
