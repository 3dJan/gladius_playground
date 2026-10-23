#include "compute/TranslucencyConfig.h"

#include <algorithm>

namespace gladius::compute
{
    TranslucencyConfig clampTranslucency(TranslucencyConfig const & config)
    {
        return {.enabled = config.enabled,
                .referenceThicknessMm = std::clamp(config.referenceThicknessMm,
                                                   MIN_REFERENCE_THICKNESS_MM,
                                                   MAX_REFERENCE_THICKNESS_MM),
                .interiorStepSizeMm = std::clamp(config.interiorStepSizeMm,
                                                 MIN_INTERIOR_STEP_MM,
                                                 MAX_INTERIOR_STEP_MM),
                .transmittanceFloor = std::clamp(config.transmittanceFloor,
                                                 MIN_TRANSMITTANCE_FLOOR,
                                                 MAX_TRANSMITTANCE_FLOOR),
                .maxInteriorSteps =
                  std::clamp(config.maxInteriorSteps, MIN_INTERIOR_STEPS, MAX_INTERIOR_STEPS),
                .maxSurfaces = std::clamp(config.maxSurfaces, MIN_SURFACES, MAX_SURFACES)};
    }

    TranslucencyConfig readTranslucency(RenderingSettings const & settings)
    {
        return {.enabled = (settings.flags & RF_ENABLE_TRANSLUCENCY) != 0,
                .referenceThicknessMm = settings.translucencyReferenceThicknessMm,
                .interiorStepSizeMm = settings.translucencyInteriorStepSize,
                .transmittanceFloor = settings.translucencyFloor,
                .maxInteriorSteps = settings.translucencyMaxInteriorSteps,
                .maxSurfaces = settings.translucencyMaxSurfaces};
    }

    bool applyTranslucency(RenderingSettings & settings, TranslucencyConfig const & config)
    {
        auto const clamped = clampTranslucency(config);
        auto const flags = clamped.enabled ? settings.flags | RF_ENABLE_TRANSLUCENCY
                                           : settings.flags & ~static_cast<int>(RF_ENABLE_TRANSLUCENCY);

        bool const changed = settings.flags != flags ||
                             settings.translucencyReferenceThicknessMm !=
                               clamped.referenceThicknessMm ||
                             settings.translucencyInteriorStepSize != clamped.interiorStepSizeMm ||
                             settings.translucencyFloor != clamped.transmittanceFloor ||
                             settings.translucencyMaxInteriorSteps != clamped.maxInteriorSteps ||
                             settings.translucencyMaxSurfaces != clamped.maxSurfaces;

        settings.flags = flags;
        settings.translucencyReferenceThicknessMm = clamped.referenceThicknessMm;
        settings.translucencyInteriorStepSize = clamped.interiorStepSizeMm;
        settings.translucencyFloor = clamped.transmittanceFloor;
        settings.translucencyMaxInteriorSteps = clamped.maxInteriorSteps;
        settings.translucencyMaxSurfaces = clamped.maxSurfaces;
        return changed;
    }

    TranslucencyConfig readTranslucency(RenderSettingsSnapshot const & settings)
    {
        return {.enabled = (settings.flags & RF_ENABLE_TRANSLUCENCY) != 0u,
                .referenceThicknessMm = settings.translucencyReferenceThicknessMm,
                .interiorStepSizeMm = settings.translucencyInteriorStepSize,
                .transmittanceFloor = settings.translucencyFloor,
                .maxInteriorSteps = static_cast<float>(settings.translucencyMaxInteriorSteps),
                .maxSurfaces = static_cast<float>(settings.translucencyMaxSurfaces)};
    }

    bool applyTranslucency(RenderSettingsSnapshot & settings, TranslucencyConfig const & config)
    {
        auto const clamped = clampTranslucency(config);
        auto const flags =
          clamped.enabled
            ? settings.flags | RF_ENABLE_TRANSLUCENCY
            : settings.flags & ~static_cast<std::uint32_t>(RF_ENABLE_TRANSLUCENCY);
        auto const maxInteriorSteps = static_cast<std::uint32_t>(clamped.maxInteriorSteps);
        auto const maxSurfaces = static_cast<std::uint32_t>(clamped.maxSurfaces);

        bool const changed =
          settings.flags != flags ||
          settings.translucencyReferenceThicknessMm != clamped.referenceThicknessMm ||
          settings.translucencyInteriorStepSize != clamped.interiorStepSizeMm ||
          settings.translucencyFloor != clamped.transmittanceFloor ||
          settings.translucencyMaxInteriorSteps != maxInteriorSteps ||
          settings.translucencyMaxSurfaces != maxSurfaces;

        settings.flags = flags;
        settings.translucencyReferenceThicknessMm = clamped.referenceThicknessMm;
        settings.translucencyInteriorStepSize = clamped.interiorStepSizeMm;
        settings.translucencyFloor = clamped.transmittanceFloor;
        settings.translucencyMaxInteriorSteps = maxInteriorSteps;
        settings.translucencyMaxSurfaces = maxSurfaces;
        return changed;
    }
} // namespace gladius::compute
