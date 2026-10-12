#pragma once

#include <algorithm>
#include <cmath>

namespace gladius::ui::async_rendering
{
    /// @brief Adjusts interactive preview quality based on render time and pixel-area scaling.
    [[nodiscard]] inline float adaptInteractivePreviewQuality(float currentQuality,
                                                              float targetFrameTimeMs,
                                                              float observedFrameTimeMs,
                                                              float minQuality,
                                                              float maxQuality) noexcept
    {
        if (!std::isfinite(currentQuality) || !std::isfinite(targetFrameTimeMs) ||
            !std::isfinite(observedFrameTimeMs) || targetFrameTimeMs <= 0.0f ||
            observedFrameTimeMs <= 0.0f)
        {
            return std::clamp(currentQuality, minQuality, maxQuality);
        }

        // Render cost approximately follows pixel area, so quality (linear dimension) scales
        // with the square root of the target/observed frame-time ratio. Limit each step to avoid
        // overreacting to a single noisy sample while still dropping quickly on slow scenes.
        float const scale = std::clamp(std::sqrt(targetFrameTimeMs / observedFrameTimeMs), 0.5f, 1.2f);
        return std::clamp(currentQuality * scale, minQuality, maxQuality);
    }
} // namespace gladius::ui::async_rendering
