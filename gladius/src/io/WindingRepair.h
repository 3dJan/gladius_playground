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

    /// @brief A representative triangle sample used to vote on component orientation.
    struct TriangleOrientationSample
    {
        Eigen::Vector3f position{Eigen::Vector3f::Zero()};
        Eigen::Vector3f normal{Eigen::Vector3f::Zero()};
        std::uint32_t component{0U};
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
    /// @param triangleComponents Optional output component ID for each triangle
    /// @return Statistics about the repair operation
    [[nodiscard]] WindingRepairStats repairTriangleWindingConsistency(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<std::uint32_t>& indices,
        std::vector<std::uint32_t>* triangleComponents = nullptr);

    /// @brief Reorients connected triangle components using signed outward-orientation votes.
    ///
    /// Each vote is the accumulated alignment between a component's current triangle normals
    /// and a reference direction (for SDF meshes, the sampled SDF gradient). Negative votes
    /// flip the entire component; zero votes leave it unchanged.
    [[nodiscard]] std::size_t orientTriangleComponentsByVotes(
        std::vector<std::uint32_t>& indices,
        std::vector<std::uint32_t> const& triangleComponents,
        std::vector<double> const& componentVotes);

    /// @brief Selects a bounded, deterministic set of non-degenerate triangles per component.
    [[nodiscard]] std::vector<TriangleOrientationSample> makeTriangleOrientationSamples(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<std::uint32_t> const& indices,
        std::vector<std::uint32_t> const& triangleComponents,
        std::size_t maximumSamplesPerComponent = 512U);

} // namespace gladius::io
