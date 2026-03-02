#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gladius::io
{
    /// Statistics from winding consistency repair.
    struct WindingRepairStats
    {
        std::size_t triangleCount{0U};
        std::size_t adjacencyConstraints{0U};
        std::size_t components{0U};
        std::size_t flippedTriangles{0U};
        bool hadInconsistency{false};
        bool flippedGlobalOrientation{false};
    };

    /// Ensures local winding consistency across shared edges by flipping triangles.
    ///
    /// This targets the class of defects where the mesh is topologically closed in an
    /// *undirected* sense (each edge has exactly two incident triangles) but triangles
    /// have inconsistent orientation, causing oriented-edge conflicts that many slicers
    /// may report as "open" or "broken" edges.
    ///
    /// @param positions Vertex positions (read-only)
    /// @param indices Triangle indices (modified in-place to ensure consistent winding)
    /// @return Statistics about the repair operation
    [[nodiscard]] WindingRepairStats repairTriangleWindingConsistency(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<std::uint32_t>& indices);

} // namespace gladius::io
