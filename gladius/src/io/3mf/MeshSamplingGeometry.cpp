/**
 * @file MeshSamplingGeometry.cpp
 * @brief Mesh-extraction-agnostic sampling geometry helpers.
 */

#include "MeshSamplingGeometry.h"

#include "Mesh.h"

#include <limits>
#include <stdexcept>

namespace gladius::io
{
    MeshSamplingGeometry MeshSamplingGeometry::fromTriangleSoupMesh(Mesh & mesh)
    {
        MeshSamplingGeometry geometry;

        std::size_t const numFaces = mesh.getNumberOfFaces();
        geometry.vertices.reserve(numFaces * 3U);
        geometry.faces.reserve(numFaces);

        auto const & vertexData = mesh.getVertices().getData();
        if (vertexData.size() < numFaces * 3U)
        {
            throw std::runtime_error("Mesh vertex buffer is smaller than the face count requires");
        }

        for (std::size_t faceIndex = 0U; faceIndex < numFaces; ++faceIndex)
        {
            if (geometry.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - 3U)
            {
                throw std::runtime_error("Mesh has too many vertices for 32-bit color sampling indices");
            }

            auto const baseIndex = static_cast<std::uint32_t>(geometry.vertices.size());
            for (std::size_t vertexIndex = 0U; vertexIndex < 3U; ++vertexIndex)
            {
                auto const & vertex = vertexData[(faceIndex * 3U) + vertexIndex];
                geometry.vertices.emplace_back(vertex.x, vertex.y, vertex.z);
            }
            geometry.faces.push_back({baseIndex, baseIndex + 1U, baseIndex + 2U});
        }

        return geometry;
    }

    MeshSamplingGeometry MeshSamplingGeometry::fromIndexedTriangles(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<std::uint32_t> const & indices)
    {
        if (indices.size() % 3U != 0U)
        {
            throw std::runtime_error("Indexed sampling geometry has incomplete triangles");
        }

        if (positions.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::runtime_error("Mesh has too many vertices for 32-bit color sampling indices");
        }

        MeshSamplingGeometry geometry;
        geometry.vertices = positions;
        geometry.faces.reserve(indices.size() / 3U);

        for (std::size_t index = 0U; index < indices.size(); index += 3U)
        {
            auto const idxA = indices[index + 0U];
            auto const idxB = indices[index + 1U];
            auto const idxC = indices[index + 2U];
            if (idxA >= positions.size() || idxB >= positions.size() || idxC >= positions.size())
            {
                throw std::runtime_error("Indexed sampling geometry references an out-of-range vertex");
            }

            geometry.faces.push_back({idxA, idxB, idxC});
        }

        return geometry;
    }

    bool MeshSamplingGeometry::matchesFaceCount(Mesh const & mesh) const
    {
        return faces.size() == mesh.getNumberOfFaces();
    }
}
