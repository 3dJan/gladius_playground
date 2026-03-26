/**
 * @file ColorExportDispatcher.cpp
 * @brief Implementation of shared color export dispatch logic
 */

#include "ColorExportDispatcher.h"

#include "ColorQuantizer.h"
#include "ColorRegionizer.h"
#include "MeshWriter3mf.h"
#include "MmuSegmentationWriter.h"

#include <algorithm>

namespace gladius::io
{

    void dispatchColorExport(
        MeshWriter3mf& writer,
        std::filesystem::path const& filePath,
        Mesh const& mesh,
        std::string const& meshName,
        FaceColors const& faceColors,
        CompatibilityDecision const& decision,
        MeshColorExportSettings const& settings,
        std::size_t uniqueColors,
        Document const* sourceDocument,
        bool writeThumbnail,
        VertexColorSupplier const& vertexColorSupplier,
        MultipointColorSupplier const& multipointSupplier)
    {
        switch (decision.finalRepresentation)
        {
        case ExportRepresentation::StandardTriangleColor:
        {
            if (settings.preferredColorMode == ColorMode::PerVertex && vertexColorSupplier)
            {
                auto vertexColors = vertexColorSupplier();
                writer.exportMeshWithVertexColors(
                    filePath, mesh, meshName, vertexColors, sourceDocument, writeThumbnail);
            }
            else
            {
                writer.exportMeshWithColors(
                    filePath, mesh, meshName, faceColors, sourceDocument, writeThumbnail);
            }
            break;
        }
        case ExportRepresentation::StandardDiscreteComponents:
        case ExportRepresentation::StandardDiscreteObjects:
        case ExportRepresentation::StandardBuildItems:
        {
            std::uint32_t const maxPalette =
                settings.maxPaletteSize.value_or(
                    static_cast<std::uint32_t>(std::min(uniqueColors, std::size_t{256})));

            auto palette = ColorQuantizer::quantize(faceColors, maxPalette);

            PrintableRegionKind regionKind = PrintableRegionKind::Component;
            if (decision.finalRepresentation == ExportRepresentation::StandardDiscreteObjects)
            {
                regionKind = PrintableRegionKind::Object;
            }
            else if (decision.finalRepresentation == ExportRepresentation::StandardBuildItems)
            {
                regionKind = PrintableRegionKind::BuildItem;
            }

            auto regions = ColorRegionizer::regionize(palette, regionKind);
            writer.exportMeshWithRegions(
                filePath, mesh, meshName, palette, regions, sourceDocument, writeThumbnail);
            break;
        }
        case ExportRepresentation::ProprietaryMmuSegmentation:
        {
            std::uint32_t const maxPalette =
                settings.maxPaletteSize.value_or(
                    static_cast<std::uint32_t>(
                        std::min(uniqueColors,
                                 static_cast<std::size_t>(
                                     MmuSegmentationWriter::MAX_EXTRUDERS))));

            if (multipointSupplier)
            {
                auto sampleSets = multipointSupplier();
                auto palette = ColorQuantizer::quantizeOversampled(sampleSets, maxPalette);
                writer.exportMeshWithMmuSegmentation(
                    filePath, mesh, meshName, palette, sourceDocument, writeThumbnail);
            }
            else
            {
                // Fallback: quantize from face colors if no multipoint supplier
                auto palette = ColorQuantizer::quantize(faceColors, maxPalette);
                writer.exportMeshWithMmuSegmentation(
                    filePath, mesh, meshName, palette, sourceDocument, writeThumbnail);
            }
            break;
        }
        default:
            writer.exportMeshWithColors(
                filePath, mesh, meshName, faceColors, sourceDocument, writeThumbnail);
            break;
        }
    }

} // namespace gladius::io
