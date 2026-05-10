/**
 * @file MeshColorExportPipeline.cpp
 * @brief Shared color sampling, planning, and 3MF export pipeline for meshes.
 */

#include "MeshColorExportPipeline.h"

#include "ColorExportDispatcher.h"
#include "ColorQuantizer.h"
#include "FaceColorSampler.h"
#include "MeshWriter3mf.h"

#include "compute/ComputeCore.h"
#include "compute/ProgramManager.h"

#include <fmt/format.h>

#include <stdexcept>

namespace gladius::io
{
    namespace
    {
        void reportProgress(MeshColorExportPipelineOptions const & options, double progress)
        {
            if (options.progressCallback)
            {
                options.progressCallback(progress);
            }
        }
    }

    ColoredMeshExportResult exportMeshWithColorPipeline(
      std::filesystem::path const & filePath,
      Mesh & mesh,
      std::string const & meshName,
      MeshSamplingGeometry const & samplingGeometry,
      ComputeCore & computeCore,
      MeshColorExportSettings const & settings,
      Document const * sourceDocument,
      bool writeThumbnail,
      events::SharedLogger const & logger,
      MeshColorExportPipelineOptions const & options)
    {
        if (!samplingGeometry.matchesFaceCount(mesh))
        {
            throw std::runtime_error(
              fmt::format("Sampling geometry face count ({}) does not match mesh face count ({})",
                          samplingGeometry.faces.size(),
                          mesh.getNumberOfFaces()));
        }

        MeshWriter3mf writer(logger);
        ColoredMeshExportResult result{};

        if (!settings.exportWithColors)
        {
            writer.exportMesh(filePath, mesh, meshName, sourceDocument, writeThumbnail);
            reportProgress(options, 1.0);
            return result;
        }

        auto * samplingProgram = computeCore.getProgramManager().getDualContouringSamplingProgram();
        auto primitives = computeCore.getPrimitives();

        if (samplingProgram == nullptr || primitives == nullptr)
        {
            std::string const warning = "Color sampling unavailable, exporting without colors";
            result.warnings.push_back(warning);

            if (logger)
            {
                logger->addEvent({warning, events::Severity::Warning});
            }

            writer.exportMesh(filePath, mesh, meshName, sourceDocument, writeThumbnail);
            reportProgress(options, 1.0);
            return result;
        }

        FaceColors faceColors =
          options.faceSamplingMode == FaceColorSamplingMode::MajorityVote
            ? FaceColorSampler::sampleFaceColorsMajorityVote(samplingGeometry.vertices,
                                                             samplingGeometry.faces,
                                                             *samplingProgram,
                                                             *primitives,
                                                             nullptr,
                                                             settings.convertToSrgb)
            : FaceColorSampler::sampleFaceColorsAsColor8(samplingGeometry.vertices,
                                                         samplingGeometry.faces,
                                                         *samplingProgram,
                                                         *primitives,
                                                         nullptr,
                                                         settings.convertToSrgb);

        reportProgress(options, 0.35);

        auto const uniqueColors = ColorQuantizer::countUniqueOpaqueColors(faceColors);
        bool const hasTransparency = ColorQuantizer::hasTransparency(faceColors);

        auto const decision = ColorCompatibilityPlanner::decide(settings, uniqueColors, hasTransparency);

        result.representation = decision.finalRepresentation;
        result.standardsOnly = !decision.needsProprietaryTags;
        result.transparencyIgnored = hasTransparency;
        result.warnings = decision.warnings;

        if (logger)
        {
            for (auto const & warning : decision.warnings)
            {
                logger->addEvent({warning, events::Severity::Warning});
            }
        }

        reportProgress(options, 0.55);

        auto vertexColorSupplier = [&]() -> VertexColors
        {
            return FaceColorSampler::sampleVertexColors(samplingGeometry.vertices,
                                                        samplingGeometry.faces,
                                                        *samplingProgram,
                                                        *primitives,
                                                        nullptr,
                                                        settings.convertToSrgb);
        };

        auto multipointSupplier = [&]() -> std::vector<FaceColors>
        {
            return FaceColorSampler::sampleFaceColorsMultipoint(samplingGeometry.vertices,
                                                                samplingGeometry.faces,
                                                                *samplingProgram,
                                                                *primitives,
                                                                nullptr,
                                                                settings.convertToSrgb);
        };

        dispatchColorExport(writer,
                            filePath,
                            mesh,
                            meshName,
                            faceColors,
                            decision,
                            settings,
                            uniqueColors,
                            sourceDocument,
                            writeThumbnail,
                            vertexColorSupplier,
                            multipointSupplier);

        reportProgress(options, 1.0);

        if (logger)
        {
            logger->addEvent(
              {fmt::format("Exported 3MF mesh '{}' with colors (representation: {}) to {}",
                           meshName,
                           static_cast<int>(decision.finalRepresentation),
                           filePath.string()),
               events::Severity::Info});
        }

        return result;
    }
}
