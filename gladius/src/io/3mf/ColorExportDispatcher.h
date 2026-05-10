/**
 * @file ColorExportDispatcher.h
 * @brief Shared dispatch logic for colored mesh export based on planner decisions
 *
 * Eliminates duplication between ManifoldDualContouringStlExporter and
 * MeshExporter3mf by centralizing the switch on ExportRepresentation.
 */

#pragma once

#include "ColorCompatibilityPlanner.h"
#include "FaceColors.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gladius
{
    class Document;
    class Mesh;
} // namespace gladius

namespace gladius::io
{
    class MeshWriter3mf;

    /// Callback type for vertex color sampling (only invoked when needed)
    using VertexColorSupplier = std::function<VertexColors()>;

    /// Callback type for multi-point face color sampling (only invoked for MMU path)
    using MultipointColorSupplier = std::function<std::vector<FaceColors>()>;

    /**
     * @brief Dispatch colored export based on a compatibility decision
     *
     * Executes the appropriate MeshWriter3mf export method for the chosen
     * representation. GPU sampling (vertex colors, multipoint) is performed
     * lazily via supplier callbacks, avoiding unnecessary work.
     *
     * @param writer The 3MF writer to use
     * @param filePath Output file path
     * @param mesh The mesh to export
     * @param meshName Human-readable mesh name
     * @param faceColors Pre-sampled per-face colors
     * @param decision Planner output describing the chosen representation
     * @param settings User export settings
     * @param uniqueColors Number of distinct opaque colors
     * @param sourceDocument Optional source document (for metadata/thumbnail)
     * @param writeThumbnail Whether to include a thumbnail
     * @param vertexColorSupplier Lazy supplier for per-vertex colors
     * @param multipointSupplier Lazy supplier for multi-point sample sets
     */
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
        MultipointColorSupplier const& multipointSupplier);

} // namespace gladius::io
