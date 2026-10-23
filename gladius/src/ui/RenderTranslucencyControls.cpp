#include "RenderTranslucencyControls.h"

#include "imgui.h"

namespace gladius::ui
{
    using compute::MAX_INTERIOR_STEP_MM;
    using compute::MAX_INTERIOR_STEPS;
    using compute::MAX_REFERENCE_THICKNESS_MM;
    using compute::MAX_SURFACES;
    using compute::MAX_TRANSMITTANCE_FLOOR;
    using compute::MIN_INTERIOR_STEP_MM;
    using compute::MIN_INTERIOR_STEPS;
    using compute::MIN_REFERENCE_THICKNESS_MM;
    using compute::MIN_SURFACES;
    using compute::MIN_TRANSMITTANCE_FLOOR;
    using compute::TranslucencyConfig;

    bool renderTranslucencyControls(TranslucencyConfig & config, float const itemWidth)
    {
        bool changed = false;

        changed |= ImGui::Checkbox("Translucency", &config.enabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
              "March through the solid and attenuate by the graph's translucency channel.\n"
              "Only the high-quality paths are translucent, the low-res preview stays opaque.");
        }

        ImGui::BeginDisabled(!config.enabled);
        ImGui::SetNextItemWidth(itemWidth);
        changed |= ImGui::SliderFloat("Ref. Thickness",
                                      &config.referenceThicknessMm,
                                      MIN_REFERENCE_THICKNESS_MM,
                                      MAX_REFERENCE_THICKNESS_MM,
                                      "%.2f mm");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
              "Wall thickness the authored translucency refers to.\n"
              "A slab of exactly this thickness absorbs exactly the authored alpha.");
        }

        ImGui::SetNextItemWidth(itemWidth);
        changed |= ImGui::SliderFloat("Interior Step",
                                      &config.interiorStepSizeMm,
                                      MIN_INTERIOR_STEP_MM,
                                      MAX_INTERIOR_STEP_MM,
                                      "%.2f mm");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
              "Sample spacing while a ray travels through the solid.\n"
              "Smaller is more accurate, larger is faster.");
        }

        ImGui::SetNextItemWidth(itemWidth);
        changed |= ImGui::SliderFloat("Max Surfaces",
                                      &config.maxSurfaces,
                                      MIN_SURFACES,
                                      MAX_SURFACES,
                                      "%.0f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Highest number of shaded shells per ray.");
        }

        ImGui::SetNextItemWidth(itemWidth);
        changed |= ImGui::SliderFloat("Transmittance Floor",
                                      &config.transmittanceFloor,
                                      MIN_TRANSMITTANCE_FLOOR,
                                      MAX_TRANSMITTANCE_FLOOR,
                                      "%.3f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Accumulation stops once the remaining transmittance is lower.");
        }

        ImGui::SetNextItemWidth(itemWidth);
        changed |= ImGui::SliderFloat("Max Interior Steps",
                                      &config.maxInteriorSteps,
                                      MIN_INTERIOR_STEPS,
                                      MAX_INTERIOR_STEPS,
                                      "%.0f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Guard against rays that never leave the solid.");
        }
        ImGui::EndDisabled();

        return changed;
    }
} // namespace gladius::ui
