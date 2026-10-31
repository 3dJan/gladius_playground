#pragma once

#include "ColorCompatibilityPlanner.h"
#include "ColorQuantizer.h"
#include "io/IndexedTriangleMesh.h"
#include "io/SurfaceExtractionOptions.h"

#include <functional>
#include <optional>
#include <string>

namespace gladius::io
{
    /// Fixed palette and mutable per-face material IDs for discrete-color simplification.
    struct PreparedDiscreteColorExport
    {
        CompatibilityDecision decision;
        QuantizedPalette palette;
        bool transparencyIgnored{false};
    };

    /// Whether the representation assigns one discrete printable material per triangle.
    [[nodiscard]] bool isDiscreteColorRepresentation(ExportRepresentation representation);

    /// Resolve the existing palette limits, clamping overrides to the writer's capacity.
    [[nodiscard]] std::uint32_t resolveDiscretePaletteLimit(
        MeshColorExportSettings const & settings, std::size_t uniqueColors, bool mmu);

    /// Preserve the requested options separately while disabling extraction's internal QEM.
    [[nodiscard]] ManifoldDualContouringOptions makeUnsimplifiedColorExtractionOptions(
        ManifoldDualContouringOptions const & requested);

    /// Prepare once from the original faces; multipoint samples vote against the fixed palette.
    [[nodiscard]] PreparedDiscreteColorExport prepareDiscreteColorExport(
        MeshColorExportSettings const & settings,
        FaceColors const & faceColors,
        std::vector<FaceColors> const & sampleSets = {});

    /// Check cardinality and material IDs before simplification or serialization.
    void validateDiscretePalette(QuantizedPalette const & palette, std::size_t faceCount);

    /// Simplify only material interiors; return a warning if the requested reduction is limited
    /// or the requested algorithm cannot preserve colors. Cancellation throws.
    [[nodiscard]] std::optional<std::string> simplifyColoredMesh(
        IndexedTriangleMesh & mesh,
        QuantizedPalette & palette,
        ManifoldDualContouringOptions const & options,
        std::function<void()> throwOnCancel = nullptr,
        std::function<void(int)> progressFn = nullptr);
}