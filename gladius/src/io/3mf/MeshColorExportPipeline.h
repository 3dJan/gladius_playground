/**
 * @file MeshColorExportPipeline.h
 * @brief Shared color sampling, planning, and 3MF export pipeline for meshes.
 */

#pragma once

#include "ColorCompatibilityPlanner.h"
#include "MeshSamplingGeometry.h"

#include "EventLogger.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class Document;
    class Mesh;
}

namespace gladius::io
{
    /// Final export-state record produced by the mesh color export pipeline.
    struct ColoredMeshExportResult
    {
        ExportRepresentation representation = ExportRepresentation::StandardTriangleColor;
        bool standardsOnly = true;
        bool transparencyIgnored = false;
        std::vector<std::string> warnings;
    };

    /// Face color sampling mode used before compatibility planning.
    enum class FaceColorSamplingMode
    {
        Centroid,
        MajorityVote
    };

    /// Optional behavior for the shared color export pipeline.
    struct MeshColorExportPipelineOptions
    {
        FaceColorSamplingMode faceSamplingMode = FaceColorSamplingMode::Centroid;
        std::function<void(double)> progressCallback;
    };

    /**
     * @brief Export a mesh with optional volumetric colors using the shared color pipeline.
     *
     * Geometry extraction is intentionally not part of this function. Callers pass
     * the final mesh and matching sampling geometry, making color support agnostic
     * to OpenVDB, manifold dual contouring, or future surface extractors.
     */
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
      MeshColorExportPipelineOptions const & options = {});
}
