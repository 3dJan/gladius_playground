/**
 * @file ColorCompatibilityPlanner.cpp
 * @brief Implementation of the standards-first color compatibility decision engine
 */

#include "ColorCompatibilityPlanner.h"

namespace gladius::io
{

    CompatibilityProfile ColorCompatibilityPlanner::buildProfile(TargetApplication target)
    {
        CompatibilityProfile profile;

        switch (target)
        {
        case TargetApplication::PrusaSlicer:
            [[fallthrough]];
        case TargetApplication::Orca:
            profile.requiresPrintableRegions = true;
            profile.supportsTextureForPrintableRegions = false;
            profile.supportsVertexColorForPrintableRegions = false;
            profile.supportsTriangleColorForPrintableRegions = false;
            profile.supportsDiscreteComponents = true;
            profile.supportsDiscreteObjects = true;
            profile.supportsBuildItems = true;
            profile.supportsMmuSegmentation = true;
            profile.allowsProprietaryTags = true;
            break;

        case TargetApplication::None:
        default:
            profile.requiresPrintableRegions = true;
            profile.supportsTextureForPrintableRegions = false;
            profile.supportsVertexColorForPrintableRegions = false;
            profile.supportsTriangleColorForPrintableRegions = false;
            profile.supportsDiscreteComponents = true;
            profile.supportsDiscreteObjects = true;
            profile.supportsBuildItems = true;
            profile.allowsProprietaryTags = false;
            break;
        }

        return profile;
    }

    CompatibilityDecision ColorCompatibilityPlanner::decide(MeshColorExportSettings const& settings,
                                                            std::size_t uniqueColorCount,
                                                            bool hasTransparency)
    {
        CompatibilityDecision decision;

        auto const profile = buildProfile(settings.targetApplication);

        // Emit transparency warning if alpha data was present
        if (hasTransparency)
        {
            decision.warnings.emplace_back(
                "Transparency data was ignored for printable-region planning. "
                "Alpha values are not preserved in the exported result.");
        }

        // Walk the canonical representation ladder:
        // Texture → Vertex → Triangle → Components → Objects → Build Items

        // 1. Texture (currently not supported for printable regions in target slicers)
        if (profile.supportsTextureForPrintableRegions)
        {
            decision.finalRepresentation = ExportRepresentation::StandardTexture;
            return decision;
        }

        // 2. Vertex color (currently not supported for printable regions in target slicers)
        if (profile.supportsVertexColorForPrintableRegions)
        {
            decision.finalRepresentation = ExportRepresentation::StandardVertexColor;
            return decision;
        }

        // 3. Triangle color (currently not supported for printable regions in target slicers)
        if (profile.supportsTriangleColorForPrintableRegions)
        {
            decision.finalRepresentation = ExportRepresentation::StandardTriangleColor;
            return decision;
        }

        // 4-6. Discrete fallback path: need quantization and regionization
        bool const needsQuantization =
            settings.quantizationMode == QuantizationMode::Adaptive && uniqueColorCount > 1;

        // Determine max palette size
        std::uint32_t const effectiveMaxPalette =
            settings.maxPaletteSize.value_or(static_cast<std::uint32_t>(uniqueColorCount));

        bool const quantizationRequired = uniqueColorCount > effectiveMaxPalette;

        if (quantizationRequired && settings.quantizationMode == QuantizationMode::Disabled)
        {
            decision.warnings.emplace_back(
                "Quantization is disabled but the model has more unique colors than the "
                "palette limit. Color fidelity may be reduced.");
        }

        decision.needsQuantization = needsQuantization && quantizationRequired;
        decision.needsRegionization = true;

        // 4. Proprietary MMU segmentation (best per-triangle fidelity for Slic3r-based slicers)
        if (profile.supportsMmuSegmentation)
        {
            decision.finalRepresentation = ExportRepresentation::ProprietaryMmuSegmentation;
            decision.needsRegionization = false; // single mesh, no region splitting needed
            decision.needsProprietaryTags = true;

            if (decision.needsQuantization)
            {
                decision.warnings.emplace_back(
                    "Color detail was reduced to fit extruder count. "
                    "Adaptive quantization was applied.");
            }

            decision.warnings.emplace_back(
                "Using proprietary MMU segmentation format for per-triangle color. "
                "The file contains slicer-specific extensions.");

            return decision;
        }

        // 5. Discrete components
        if (profile.supportsDiscreteComponents)
        {
            decision.finalRepresentation = ExportRepresentation::StandardDiscreteComponents;

            if (decision.needsQuantization)
            {
                decision.warnings.emplace_back(
                    "Color detail was reduced to preserve printable regions. "
                    "Adaptive quantization was applied.");
            }

            return decision;
        }

        // 5. Discrete objects
        if (profile.supportsDiscreteObjects)
        {
            decision.finalRepresentation = ExportRepresentation::StandardDiscreteObjects;
            return decision;
        }

        // 6. Build items (lowest-fidelity standards-only fallback)
        if (profile.supportsBuildItems)
        {
            decision.finalRepresentation = ExportRepresentation::StandardBuildItems;
            decision.warnings.emplace_back(
                "Printable-region fidelity was reduced to build-item level. "
                "Individual triangles may not retain distinct color assignments.");
            return decision;
        }

        // 7. Proprietary path: only when user explicitly selected a target
        if (settings.targetApplication != TargetApplication::None && profile.allowsProprietaryTags)
        {
            decision.finalRepresentation = ExportRepresentation::ProprietaryTargetTagged;
            decision.needsProprietaryTags = true;
            decision.warnings.emplace_back(
                "Proprietary slicer-specific tags were added for the selected target. "
                "Portability to other slicers may be reduced.");
            return decision;
        }

        // Standards-limited fallback
        decision.finalRepresentation = ExportRepresentation::StandardBuildItems;
        decision.warnings.emplace_back(
            "Standard-only export could not fully preserve printable regions. "
            "Select a target application to enable slicer-specific optimization.");

        return decision;
    }

} // namespace gladius::io
