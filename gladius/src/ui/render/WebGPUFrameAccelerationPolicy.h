#pragma once

#include "../../compute/RenderContracts.h"
#include "../../kernel/types.h"
#include "RenderUpdateTypes.h"

#include <algorithm>
#include <cstdint>

namespace gladius::ui::async_rendering
{
    /// Pure policy deciding which ray-march accelerators a WebGPU frame task may use.
    ///
    /// The accelerators split into two groups:
    /// * Bounds guards (`RF_SKIP_BACKGROUND_RAYS`, `RF_TILE_CULLING`) provably skip work
    ///   without changing any pixel, so they are safe on every frame kind.
    /// * Preview accelerators (`RF_DISABLE_SHADOWS`, `RF_DISABLE_AO`, `RF_COARSE_REFINEMENT`)
    ///   trade shading fidelity for interactive speed, so they are only allowed on
    ///   `LowResolutionPreview`.
    ///
    /// `RealtimeFullFrame`, `StaticFullFrameProbe` and `ProgressiveHighQualityChunk` all
    /// feed the high-quality frame stamp, so they keep the exact march. Non-frame task
    /// kinds get no accelerators at all.
    [[nodiscard]] constexpr std::uint32_t chooseFrameAccelerationFlags(
      RenderTaskType type) noexcept
    {
        constexpr auto boundsGuards =
          RF_SKIP_BACKGROUND_RAYS | RF_TILE_CULLING | RF_USE_PRECOMPUTED_SDF;
        constexpr auto previewFlags = RF_DISABLE_SHADOWS | RF_DISABLE_AO | RF_COARSE_REFINEMENT;

        switch (type)
        {
        case RenderTaskType::LowResolutionPreview:
            return boundsGuards | previewFlags;
        case RenderTaskType::RealtimeFullFrame:
        case RenderTaskType::StaticFullFrameProbe:
        case RenderTaskType::ProgressiveHighQualityChunk:
            return boundsGuards;
        case RenderTaskType::StreamingPreview:
        case RenderTaskType::ProgramCompilation:
        case RenderTaskType::ParameterUpload:
        case RenderTaskType::BoundingBoxUpdate:
        case RenderTaskType::SdfPrecomputation:
            return 0u;
        }

        return 0u;
    }

    /// Produces the effective frame render settings for a given WebGPU task type.
    ///
    /// High-quality tasks keep `baseSettings` intact apart from lossless bounds guards.
    /// `LowResolutionPreview` additionally disables translucency, shadows, and AO and
    /// caps ray-march steps so parameter and camera edits remain responsive on complex
    /// models and weaker GPUs.
    [[nodiscard]] constexpr compute::RenderSettingsSnapshot applyFrameAccelerationSettings(
      compute::RenderSettingsSnapshot const & baseSettings,
      RenderTaskType type) noexcept
    {
        auto settings = baseSettings;
        settings.flags |= chooseFrameAccelerationFlags(type);
        if (type == RenderTaskType::LowResolutionPreview)
        {
            constexpr std::uint32_t previewMaxRaySteps = 96u;
            constexpr float previewMinNormalOffset = 0.0015f;
            constexpr float previewMaxQuality = 0.5f;

            settings.flags &= ~RF_ENABLE_TRANSLUCENCY;
            settings.maxRaySteps = std::min(settings.maxRaySteps, previewMaxRaySteps);
            settings.normalOffset = std::max(settings.normalOffset, previewMinNormalOffset);
            settings.quality = std::min(settings.quality, previewMaxQuality);
        }
        return settings;
    }
}
