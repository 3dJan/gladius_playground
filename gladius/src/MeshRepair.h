#pragma once

/// @file MeshRepair.h
/// @brief Pre-BVH mesh cleanup operations for robust SDF evaluation.
/// @details Pure functions operating on host-side vertex/index buffers. Each
///          step is independent and opt-in via @ref MeshRepairConfig. Repair
///          runs once at mesh import time (see io/3mf/Importer3mf.cpp) so it
///          incurs no per-frame cost.

#include "MeshBVH.h"

#include <cstddef>
#include <vector>

namespace gladius::mesh_repair
{
    /// Configuration for a mesh repair pass. Each step is independently togglable.
    /// Defaults are all-disabled to preserve the legacy import behaviour.
    struct MeshRepairConfig
    {
        /// Merge vertices closer than @ref weldEpsilon and remap indices.
        bool weld = false;
        /// Spatial threshold (mm) below which vertices are merged. Reasonable
        /// values are O(1e-5) of the mesh bounding-box diagonal.
        float weldEpsilon = 1e-5f;

        /// Drop triangles whose area is below @ref areaEpsilon.
        bool removeDegenerate = false;
        /// Triangle area threshold (mm²) below which a triangle is dropped.
        float areaEpsilon = 1e-10f;

        /// Flip individual triangles so each connected manifold component
        /// has a consistent outward orientation (majority area vote per
        /// component; flood-fill on edge adjacency).
        bool orientConsistently = false;

        /// Triangulate boundary loops whose perimeter is below
        /// @ref maxHolePerimeter. Larger boundaries are left open
        /// (treated as intentional openings).
        bool fillHoles = false;
        /// Boundary-loop perimeter threshold (mm). Loops above this length
        /// are not filled.
        float maxHolePerimeter = 1.0f;
    };

    /// Per-step counters returned by @ref repairMesh. Useful for logging and tests.
    struct MeshRepairResult
    {
        std::size_t weldedVertices = 0;     ///< Number of vertex slots eliminated by welding.
        std::size_t removedTriangles = 0;   ///< Number of triangles dropped as degenerate.
        std::size_t flippedTriangles = 0;   ///< Number of triangles re-oriented.
        std::size_t filledHoles = 0;        ///< Number of boundary loops triangulated.
        std::size_t addedTriangles = 0;     ///< Number of triangles added by hole filling.
    };

    /// Merge spatially coincident vertices.
    /// @param vertices In/out vertex array. After return, may contain fewer entries.
    /// @param indices In/out triangle indices, remapped to point at the merged vertices.
    /// @param epsilon Maximum L∞ distance between vertices considered identical.
    /// @return Number of vertex slots removed.
    std::size_t weldVertices(std::vector<float4> & vertices,
                             std::vector<TriangleIndices> & indices,
                             float epsilon);

    /// Drop triangles with area below the threshold (zero-area or sliver fragments).
    /// Indices are not remapped — vertex array is unchanged.
    /// @return Number of triangles removed.
    std::size_t removeDegenerateTriangles(std::vector<float4> const & vertices,
                                          std::vector<TriangleIndices> & indices,
                                          float areaEpsilon);

    /// Flip individual triangles so every connected component has a consistent
    /// outward orientation. Uses BFS over the face-adjacency graph, then a
    /// per-component majority vote weighted by face area.
    /// @return Number of triangles whose winding was reversed.
    std::size_t orientConsistently(std::vector<float4> const & vertices,
                                   std::vector<TriangleIndices> & indices);

    /// Triangulate small boundary loops with a fan over the loop centroid.
    /// @param vertices In/out vertex array. Centroid vertices may be appended.
    /// @param indices In/out triangle indices. New triangles are appended.
    /// @param maxPerimeter Loops with perimeter > this value are left open.
    /// @param[out] outFilled Number of boundary loops successfully filled.
    /// @param[out] outAdded Number of triangles added.
    void fillSmallHoles(std::vector<float4> & vertices,
                        std::vector<TriangleIndices> & indices,
                        float maxPerimeter,
                        std::size_t & outFilled,
                        std::size_t & outAdded);

    /// Run all enabled repair steps in a sensible order:
    /// weld → removeDegenerate → orientConsistently → fillSmallHoles.
    MeshRepairResult repairMesh(std::vector<float4> & vertices,
                                std::vector<TriangleIndices> & indices,
                                MeshRepairConfig const & config);

} // namespace gladius::mesh_repair
