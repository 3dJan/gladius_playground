#include "PreparedDiscreteColorExport.h"

#include "MmuSegmentationWriter.h"
#include "compute/FastQemSimplification.h"

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>

namespace gladius::io
{
    bool isDiscreteColorRepresentation(ExportRepresentation representation)
    {
        return representation == ExportRepresentation::ProprietaryMmuSegmentation ||
               representation == ExportRepresentation::StandardDiscreteComponents ||
               representation == ExportRepresentation::StandardDiscreteObjects ||
               representation == ExportRepresentation::StandardBuildItems;
    }

    std::uint32_t resolveDiscretePaletteLimit(
        MeshColorExportSettings const & settings, std::size_t uniqueColors, bool mmu)
    {
        auto const capacity = mmu ? MmuSegmentationWriter::MAX_EXTRUDERS : 256U;
        auto const defaultLimit = static_cast<std::uint32_t>(std::min(uniqueColors, std::size_t{capacity}));
        return std::clamp(settings.maxPaletteSize.value_or(defaultLimit), 1U, capacity);
    }

    ManifoldDualContouringOptions makeUnsimplifiedColorExtractionOptions(
        ManifoldDualContouringOptions const & requested)
    {
        auto options = requested;
        options.simplificationMethod = SimplificationMethod::None;
        options.enableSimplification = false;
        return options;
    }

    void validateDiscretePalette(QuantizedPalette const & palette, std::size_t faceCount)
    {
        if (palette.sourceToPaletteMap.size() != faceCount ||
            (faceCount != 0U && palette.colors.empty()))
        {
            throw std::invalid_argument("Discrete palette does not match the mesh face count");
        }
        for (auto const material : palette.sourceToPaletteMap)
        {
            if (material >= palette.colors.size())
            {
                throw std::invalid_argument("Discrete palette contains an out-of-range material ID");
            }
        }
    }

    PreparedDiscreteColorExport prepareDiscreteColorExport(
        MeshColorExportSettings const & settings,
        FaceColors const & faceColors,
        std::vector<FaceColors> const & sampleSets)
    {
        if (faceColors.empty())
        {
            throw std::invalid_argument("Cannot prepare discrete colors for an empty mesh");
        }
        for (auto const & samples : sampleSets)
        {
            if (samples.size() != faceColors.size())
            {
                throw std::invalid_argument("Color sample sets must match the original mesh face count");
            }
        }
        auto const uniqueColors = ColorQuantizer::countUniqueOpaqueColors(faceColors);
        bool const transparency = ColorQuantizer::hasTransparency(faceColors);
        auto decision = ColorCompatibilityPlanner::decide(settings, uniqueColors, transparency);
        if (!isDiscreteColorRepresentation(decision.finalRepresentation))
        {
            throw std::invalid_argument("Prepared palettes require a discrete-color export representation");
        }
        bool const mmu = decision.finalRepresentation == ExportRepresentation::ProprietaryMmuSegmentation;
        auto const limit = resolveDiscretePaletteLimit(settings, uniqueColors, mmu);
        auto effectiveSettings = settings;
        effectiveSettings.maxPaletteSize = limit;
        decision = ColorCompatibilityPlanner::decide(effectiveSettings, uniqueColors, transparency);
        PreparedDiscreteColorExport result;
        result.decision = std::move(decision);
        result.transparencyIgnored = transparency;
        result.palette = sampleSets.empty() ? ColorQuantizer::quantize(faceColors, limit)
                                            : ColorQuantizer::quantizeOversampled(sampleSets, limit);
        validateDiscretePalette(result.palette, faceColors.size());
        return result;
    }

    std::optional<std::string> simplifyColoredMesh(
        IndexedTriangleMesh & mesh,
        QuantizedPalette & palette,
        ManifoldDualContouringOptions const & options,
        std::function<void()> throwOnCancel,
        std::function<void(int)> progressFn)
    {
        if (throwOnCancel) throwOnCancel();
        validateDiscretePalette(palette, mesh.indices.size() / 3U);
        if (options.simplificationMethod == SimplificationMethod::None && !options.enableSimplification)
        {
            return std::nullopt;
        }
        if (options.simplificationMethod != SimplificationMethod::QemFast)
        {
            return "SDF-aware simplification was skipped to preserve color boundaries; the colored mesh remains unsimplified";
        }
        compute::FastQemConfig config;
        config.targetTriangleCount = options.simplificationTargetTriangles.value_or(0U);
        config.targetReductionPercent = options.simplificationTargetReduction.value_or(50.0F);
        config.maxError = options.simplificationMaxError;
        switch (options.simplificationTerminationMode)
        {
        case SimplificationTerminationMode::TargetTriangleCount:
            config.terminationMode = compute::SimplificationTerminationMode::TargetTriangleCount;
            break;
        case SimplificationTerminationMode::TargetReductionPercent:
            config.terminationMode = compute::SimplificationTerminationMode::TargetReductionPercent;
            break;
        case SimplificationTerminationMode::ErrorBounded:
            config.terminationMode = compute::SimplificationTerminationMode::ErrorBounded;
            break;
        }
        auto const before = computeIndexedTriangleMeshDiagnostics(mesh);
        compute::fastQemSimplify(mesh.positions, mesh.indices, palette.sourceToPaletteMap,
                                 config, throwOnCancel, progressFn);
        mesh.normals.clear();
        validateDiscretePalette(palette, mesh.indices.size() / 3U);
        if (auto const error = mesh.validationError())
        {
            throw std::runtime_error("Color-aware simplification produced an invalid mesh: " + *error);
        }
        auto const after = computeIndexedTriangleMeshDiagnostics(mesh);
        if (after.boundaryEdgeCount > before.boundaryEdgeCount ||
            after.nonManifoldEdgeCount > before.nonManifoldEdgeCount ||
            after.inconsistentWindingEdgeCount > before.inconsistentWindingEdgeCount)
        {
            throw std::runtime_error("Color-aware simplification introduced a mesh topology defect");
        }
        if (options.simplificationTerminationMode != SimplificationTerminationMode::ErrorBounded)
        {
            auto const target = options.simplificationTerminationMode == SimplificationTerminationMode::TargetTriangleCount
                                  ? config.targetTriangleCount
                                  : static_cast<std::size_t>(static_cast<float>(before.triangleCount) *
                                                            (1.0F - config.targetReductionPercent / 100.0F));
            if (after.triangleCount > target)
            {
                return fmt::format("Color-aware simplification retained {} of {} triangles (requested {}); "
                                   "material boundaries, topology and error limits take priority",
                                   after.triangleCount, before.triangleCount, target);
            }
        }
        return std::nullopt;
    }
}