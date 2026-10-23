#pragma once

#include "compute/RenderContracts.h"
#include "kernel/types.h"

namespace gladius::compute
{
    /// Slider bounds shared by the UI widgets and the MCP input clamping.
    inline constexpr float MIN_REFERENCE_THICKNESS_MM = 0.05f;
    inline constexpr float MAX_REFERENCE_THICKNESS_MM = 5.0f;
    inline constexpr float MIN_INTERIOR_STEP_MM = 0.01f;
    inline constexpr float MAX_INTERIOR_STEP_MM = 2.0f;
    inline constexpr float MIN_TRANSMITTANCE_FLOOR = 0.001f;
    inline constexpr float MAX_TRANSMITTANCE_FLOOR = 0.5f;
    inline constexpr float MIN_INTERIOR_STEPS = 8.0f;
    inline constexpr float MAX_INTERIOR_STEPS = 4096.0f;
    inline constexpr float MIN_SURFACES = 1.0f;
    inline constexpr float MAX_SURFACES = 8.0f;

    /**
     * @brief Backend-neutral translucency configuration requested by a caller.
     *
     * The high-quality raymarchers march through the solid and attenuate the accumulated colour
     * by the graph's translucency channel. That needs RF_ENABLE_TRANSLUCENCY plus the knobs
     * describing how the interior is integrated, and both the OpenCL rendering settings and the
     * backend-neutral snapshot carry them - so UI widgets and MCP input are written once against
     * this plain value instead of against either settings struct.
     */
    struct TranslucencyConfig
    {
        bool enabled{};
        /// Beer-Lambert reference thickness (mm) the authored alpha refers to.
        float referenceThicknessMm{0.4f};
        /// Step size (mm) used while a ray travels through the solid.
        float interiorStepSizeMm{0.1f};
        /// Remaining transmittance below which accumulation stops.
        float transmittanceFloor{0.01f};
        /// Upper bound on interior steps per shell.
        float maxInteriorSteps{512.f};
        /// Upper bound on shaded surfaces per ray.
        float maxSurfaces{3.f};
    };

    /// @brief Clamp every knob into the range the shaders tolerate.
    [[nodiscard]] TranslucencyConfig clampTranslucency(TranslucencyConfig const & config);

    /// @brief Mirror the translucency part of the OpenCL rendering settings.
    [[nodiscard]] TranslucencyConfig readTranslucency(RenderingSettings const & settings);

    /**
     * @brief Apply the configuration to the OpenCL rendering settings.
     * @return true when anything changed, i.e. the view has to be invalidated
     */
    bool applyTranslucency(RenderingSettings & settings, TranslucencyConfig const & config);

    /// @brief Mirror the translucency part of a backend-neutral snapshot.
    [[nodiscard]] TranslucencyConfig readTranslucency(RenderSettingsSnapshot const & settings);

    /**
     * @brief Apply the configuration to a backend-neutral snapshot.
     * @return true when anything changed, i.e. the view has to be invalidated
     */
    bool applyTranslucency(RenderSettingsSnapshot & settings, TranslucencyConfig const & config);
} // namespace gladius::compute
