#pragma once

namespace gladius::ui
{
    struct WebGpuContourRequestInput
    {
        bool parametersChanged{false};
        bool generationInFlight{false};
        bool cacheValid{false};
        bool throttleElapsed{false};
        bool modelBoundsAvailable{false};
    };

    struct WebGpuContourRequestDecision
    {
        bool shouldGenerate{false};
        bool useBuildVolumeFallback{false};
    };

    [[nodiscard]] constexpr WebGpuContourRequestDecision decideWebGpuContourRequest(
      WebGpuContourRequestInput const & input) noexcept
    {
        bool const shouldGenerate = input.parametersChanged && !input.generationInFlight &&
                                    (input.cacheValid || input.throttleElapsed);
        return {.shouldGenerate = shouldGenerate,
                .useBuildVolumeFallback = shouldGenerate && !input.modelBoundsAvailable};
    }
}
