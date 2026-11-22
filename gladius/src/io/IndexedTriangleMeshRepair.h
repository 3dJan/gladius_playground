#pragma once

#include "io/IndexedTriangleMesh.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gladius::io
{
    /// @brief Counters describing a degenerate-triangle collapse run.
    struct IndexedTriangleMeshRepairResult
    {
        std::size_t collapsedEdgeCount{0U};
        std::size_t removedTriangleCount{0U};
        std::size_t removedVertexCount{0U};
        /// @brief Degenerate triangles that are still present after the last pass.
        std::size_t remainingDegenerateTriangleCount{0U};
    };

    /// @brief Removes geometrically degenerate triangles by collapsing their shortest edge.
    /// @details Slivers are identified with the same predicate that
    ///        @ref IndexedTriangleMesh::validationError uses in strict mode. Each sliver is removed by
    ///        identifying the two endpoints of its shortest edge, which keeps the surface closed: the
    ///        faces that fold onto the collapsed edge disappear and their neighbours take over the
    ///        formerly shared edges. Faces are never dropped on their own, because that would open a
    ///        boundary, and vertices are never welded by distance, because dual contouring duplicates
    ///        coincident vertices on purpose to keep touching sheets topologically separate.
    /// @param mesh Mesh repaired in place. Untouched when it contains no degenerate triangle.
    /// @return Counters describing the collapse passes.
    [[nodiscard]] IndexedTriangleMeshRepairResult collapseGeometricallyDegenerateTriangles(
        IndexedTriangleMesh& mesh);

    /// @brief Removes geometrically degenerate triangles and keeps per-triangle labels in lockstep.
    /// @details Material and colour assignments are stored per triangle, so a collapse that folds a
    ///          face away also drops its label and the surviving faces keep their order. This keeps
    ///          arrays such as @ref QuantizedPalette::sourceToPaletteMap usable after the repair.
    ///          The label array is only touched when its size matches the triangle count.
    /// @param mesh Mesh repaired in place.
    /// @param triangleLabels One entry per triangle, rebuilt to match the surviving triangles.
    /// @return Counters describing the collapse passes.
    [[nodiscard]] IndexedTriangleMeshRepairResult collapseGeometricallyDegenerateTriangles(
        IndexedTriangleMesh& mesh,
        std::vector<std::uint32_t>& triangleLabels);

    /// @brief Counts triangles that violate the strict geometric validation predicate.
    [[nodiscard]] std::size_t countGeometricallyDegenerateTriangles(IndexedTriangleMesh const& mesh);
}
