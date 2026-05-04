/**
 * @file MeshSamplingGeometry.h
 * @brief Mesh-extraction-agnostic sampling geometry for volumetric color export.
 */

#pragma once

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace gladius
{
    class Mesh;
}

namespace gladius::io
{
    /**
     * @brief Triangle geometry used for color sampling.
     *
     * The face order must match the final exported mesh triangle order. This keeps
     * sampled face colors independent of the surface extraction method while still
     * aligned with the triangles written to 3MF.
     */
    struct MeshSamplingGeometry
    {
        std::vector<Eigen::Vector3f> vertices;
        std::vector<std::array<std::uint32_t, 3>> faces;

        /// Create sampling geometry from a triangle-soup Mesh.
        static MeshSamplingGeometry fromTriangleSoupMesh(Mesh & mesh);

        /// Create sampling geometry from indexed triangle data.
        static MeshSamplingGeometry fromIndexedTriangles(
          std::vector<Eigen::Vector3f> const & positions,
          std::vector<std::uint32_t> const & indices);

        /// Check whether this geometry has the same number of faces as the mesh.
        [[nodiscard]] bool matchesFaceCount(Mesh const & mesh) const;
    };
}
