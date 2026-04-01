/**
 * @file ColorCompatibilityPlanner.h
 * @brief Standards-first color compatibility decision engine for 3MF export
 *
 * Evaluates the canonical representation order (Texture → Vertex → Triangle →
 * Component/Object → Build Item) and produces a deterministic compatibility
 * decision that keeps the export standards-only by default.
 */

#pragma once

#include "FaceColors.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gladius::io
{

    /// Export representation selected by the compatibility planner
    enum class ExportRepresentation
    {
        StandardTexture,
        StandardVertexColor,
        StandardTriangleColor,
        StandardDiscreteComponents,
        StandardDiscreteObjects,
        StandardBuildItems,
        ProprietaryMmuSegmentation, ///< Per-triangle extruder via Slic3r MMU segmentation
        ProprietaryTargetTagged
    };

    /// Quantization behavior for color simplification
    enum class QuantizationMode
    {
        Disabled,
        Adaptive
    };

    /// Target slicer application for optional proprietary tagging
    enum class TargetApplication
    {
        None,
        PrusaSlicer,
        Orca
    };

    /// Internal rule set describing what representations are usable for a compatibility target
    struct CompatibilityProfile
    {
        bool requiresPrintableRegions = true;
        bool supportsTextureForPrintableRegions = false;
        bool supportsVertexColorForPrintableRegions = false;
        bool supportsTriangleColorForPrintableRegions = false;
        bool supportsDiscreteComponents = true;
        bool supportsDiscreteObjects = true;
        bool supportsBuildItems = true;
        bool supportsMmuSegmentation = false; ///< PrusaSlicer/OrcaSlicer per-triangle extruder
        bool allowsProprietaryTags = false;
    };

    /// Planner output describing the chosen export representation
    struct CompatibilityDecision
    {
        ExportRepresentation finalRepresentation = ExportRepresentation::StandardTriangleColor;
        bool needsQuantization = false;
        bool needsRegionization = false;
        bool needsProprietaryTags = false;
        std::vector<std::string> warnings;
    };

    /// User-controlled settings captured from the export dialog
    struct MeshColorExportSettings
    {
        bool exportWithColors = true;
        bool convertToSrgb = true;
        ColorMode preferredColorMode = ColorMode::PerFace;
        QuantizationMode quantizationMode = QuantizationMode::Adaptive;
        TargetApplication targetApplication = TargetApplication::None;
        std::optional<std::uint32_t> maxPaletteSize;
    };

    /**
     * @class ColorCompatibilityPlanner
     * @brief Standards-first decision engine for colored mesh export
     *
     * Evaluates the canonical ladder of standard 3MF representations and
     * selects the highest-fidelity option that preserves printable regions
     * in both target slicers. If no standard representation is sufficient
     * and a target application is selected, allows proprietary tagging.
     */
    class ColorCompatibilityPlanner
    {
      public:
        /// Build a compatibility profile for the given target application
        static CompatibilityProfile buildProfile(TargetApplication target);

        /// Decide which export representation to use
        static CompatibilityDecision decide(MeshColorExportSettings const& settings,
                                            std::size_t uniqueColorCount,
                                            bool hasTransparency);
    };

} // namespace gladius::io
