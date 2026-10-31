/**
 * @file ColorExportDispatcher.cpp
 * @brief Implementation of shared color export dispatch logic
 */

#include "ColorExportDispatcher.h"

#include "ColorQuantizer.h"
#include "ColorRegionizer.h"
#include "MeshWriter3mf.h"
#include "MmuSegmentationWriter.h"
#include "Mesh.h"

#include <algorithm>
#include <stdexcept>

namespace gladius::io
{

    void dispatchPreparedColorExport(
        MeshWriter3mf & writer,
        std::filesystem::path const & filePath,
        Mesh const & mesh,
        std::string const & meshName,
        PreparedDiscreteColorExport const & prepared,
        Document const * sourceDocument,
        bool writeThumbnail)
    {
        validateDiscretePalette(prepared.palette, mesh.getNumberOfFaces());
        auto const representation = prepared.decision.finalRepresentation;
        if (!isDiscreteColorRepresentation(representation))
        {
            throw std::invalid_argument("Prepared color dispatch requires discrete materials");
        }
        if (representation == ExportRepresentation::ProprietaryMmuSegmentation)
        {
            writer.exportMeshWithMmuSegmentation(filePath, mesh, meshName, prepared.palette, sourceDocument, writeThumbnail);
            return;
        }
        auto const kind = representation == ExportRepresentation::StandardDiscreteObjects
                            ? PrintableRegionKind::Object
                            : (representation == ExportRepresentation::StandardBuildItems
                                 ? PrintableRegionKind::BuildItem : PrintableRegionKind::Component);
        auto const regions = ColorRegionizer::regionize(prepared.palette, kind);
        writer.exportMeshWithRegions(filePath, mesh, meshName, prepared.palette, regions, sourceDocument, writeThumbnail);
    }

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
            auto const maxPalette = resolveDiscretePaletteLimit(settings, uniqueColors, false);

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
            auto const maxPalette = resolveDiscretePaletteLimit(settings, uniqueColors, true);

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
