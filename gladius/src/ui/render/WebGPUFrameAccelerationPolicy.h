#pragma once

#include "../../kernel/types.h"
#include "RenderUpdateTypes.h"

#include <cstdint>

namespace gladius::ui::async_rendering
{
    /// Pure policy deciding which ray-march accelerators a WebGPU frame task may use.
    ///
    /// The accelerators split into two groups:
    /// * Bounds guards (`RF_SKIP_BACKGROUND_RAYS`, `RF_TILE_CULLING`) provably skip work
    ///   without changing any pixel, so they are safe on every frame kind.
    /// * Quality knobs (`RF_FAST_SHADOW`, `RF_FAST_AO`, `RF_COARSE_REFINEMENT`) change
    ///   shading and the surface hit position, so they are only allowed on the preview
    ///   path, whose output is allowed to be approximate.
    ///
    /// `RealtimeFullFrame`, `StaticFullFrameProbe` and `ProgressiveHighQualityChunk` all
    /// feed the high-quality frame stamp, so they keep the exact march. Non-frame task
    /// kinds get no accelerators at all.
    [[nodiscard]] constexpr std::uint32_t chooseFrameAccelerationFlags(
      RenderTaskType type) noexcept
    {
        constexpr auto boundsGuards = RF_SKIP_BACKGROUND_RAYS | RF_TILE_CULLING;
        constexpr auto qualityKnobs = RF_FAST_SHADOW | RF_FAST_AO | RF_COARSE_REFINEMENT;

        switch (type)
        {
        case RenderTaskType::LowResolutionPreview:
            return boundsGuards | qualityKnobs;
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
}
