#pragma once

namespace gladius::ui::async_rendering
{
    enum class PreviewBackend
    {
        None,
        PrecomputedSdfWithDistanceInit,
        DynamicFullModel
    };

    struct PreviewBackendInput
    {
        bool rendererReady{false};
        bool lowResTargetAvailable{false};
        bool precomputedSdfValid{false};
        bool allowDynamicFullModelFallback{true};
    };

    [[nodiscard]] constexpr PreviewBackend choosePreviewBackend(
      PreviewBackendInput const & input) noexcept
    {
        if (!input.rendererReady || !input.lowResTargetAvailable)
        {
            return PreviewBackend::None;
        }

        if (input.precomputedSdfValid)
        {
            return PreviewBackend::PrecomputedSdfWithDistanceInit;
        }

        return input.allowDynamicFullModelFallback ? PreviewBackend::DynamicFullModel
                                                   : PreviewBackend::None;
    }

    [[nodiscard]] constexpr bool previewBackendProducesDistanceInit(
      PreviewBackend backend) noexcept
    {
        return backend == PreviewBackend::PrecomputedSdfWithDistanceInit;
    }
}
