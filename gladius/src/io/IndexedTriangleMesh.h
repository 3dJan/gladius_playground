#pragma once

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::io
{
    /// @brief Geometric and topological measurements for an indexed triangle mesh.
    struct IndexedTriangleMeshDiagnostics
    {
        std::size_t vertexCount{0U};
        std::size_t referencedVertexCount{0U};
        std::size_t triangleCount{0U};
        std::size_t connectedComponentCount{0U};
        std::size_t boundaryEdgeCount{0U};
        std::size_t nonManifoldEdgeCount{0U};
        std::size_t inconsistentWindingEdgeCount{0U};
        std::size_t degenerateTriangleCount{0U};
        std::size_t invalidTriangleIndexCount{0U};
        std::size_t trailingIndexCount{0U};
        Eigen::Vector3f boundsMin{Eigen::Vector3f::Zero()};
        Eigen::Vector3f boundsMax{Eigen::Vector3f::Zero()};
        double signedVolume{0.0};
    };

    /// @brief Selects whether validation rejects geometric degeneracies.
    enum class IndexedTriangleMeshValidationPolicy
    {
        Strict,
        AllowGeometricDegenerates
    };

    /// @brief Computes ordering-independent geometric and topological diagnostics for indexed meshes.
    template <typename Mesh>
    [[nodiscard]] IndexedTriangleMeshDiagnostics computeIndexedTriangleMeshDiagnostics(Mesh const& mesh)
    {
        IndexedTriangleMeshDiagnostics result;
        result.vertexCount = mesh.positions.size();
        result.triangleCount = mesh.indices.size() / 3U;
        result.trailingIndexCount = mesh.indices.size() % 3U;

        Eigen::Vector3f const volumeOrigin = mesh.positions.empty()
                                                ? Eigen::Vector3f::Zero()
                                                : mesh.positions.front();

        std::vector<std::size_t> parents(mesh.positions.size());
        std::vector<std::size_t> ranks(mesh.positions.size(), 0U);
        std::vector<bool> referencedVertices(mesh.positions.size(), false);
        for (std::size_t vertex = 0U; vertex < parents.size(); ++vertex)
        {
            parents[vertex] = vertex;
        }

        auto findRoot = [&parents](std::size_t vertex)
        {
            std::size_t root = vertex;
            while (parents[root] != root)
            {
                root = parents[root];
            }
            while (parents[vertex] != vertex)
            {
                std::size_t const next = parents[vertex];
                parents[vertex] = root;
                vertex = next;
            }
            return root;
        };
        auto joinVertices = [&parents, &ranks, &findRoot](std::size_t first, std::size_t second)
        {
            std::size_t firstRoot = findRoot(first);
            std::size_t secondRoot = findRoot(second);
            if (firstRoot == secondRoot)
            {
                return;
            }
            if (ranks[firstRoot] < ranks[secondRoot])
            {
                std::swap(firstRoot, secondRoot);
            }
            parents[secondRoot] = firstRoot;
            if (ranks[firstRoot] == ranks[secondRoot])
            {
                ++ranks[firstRoot];
            }
        };

        struct EdgeUse
        {
            std::size_t faceCount{0U};
            std::int64_t orientation{0};
        };
        std::unordered_map<std::uint64_t, EdgeUse> edgeUses;
        edgeUses.reserve(mesh.indices.size() / 2U);

        double const floatEpsilon = std::numeric_limits<float>::epsilon();
        double const relativeAreaTolerance = floatEpsilon * floatEpsilon;
        double sixTimesSignedVolume = 0.0;
        for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
        {
            std::size_t const first = mesh.indices[offset];
            std::size_t const second = mesh.indices[offset + 1U];
            std::size_t const third = mesh.indices[offset + 2U];
            if (first >= mesh.positions.size() || second >= mesh.positions.size() ||
                third >= mesh.positions.size())
            {
                ++result.invalidTriangleIndexCount;
                continue;
            }

            referencedVertices[first] = true;
            referencedVertices[second] = true;
            referencedVertices[third] = true;
            joinVertices(first, second);
            joinVertices(second, third);

            auto const& a = mesh.positions[first];
            auto const& b = mesh.positions[second];
            auto const& c = mesh.positions[third];
            double const abx = static_cast<double>(b.x()) - a.x();
            double const aby = static_cast<double>(b.y()) - a.y();
            double const abz = static_cast<double>(b.z()) - a.z();
            double const acx = static_cast<double>(c.x()) - a.x();
            double const acy = static_cast<double>(c.y()) - a.y();
            double const acz = static_cast<double>(c.z()) - a.z();
            double const bcx = static_cast<double>(c.x()) - b.x();
            double const bcy = static_cast<double>(c.y()) - b.y();
            double const bcz = static_cast<double>(c.z()) - b.z();
            double const abSquared = abx * abx + aby * aby + abz * abz;
            double const acSquared = acx * acx + acy * acy + acz * acz;
            double const bcSquared = bcx * bcx + bcy * bcy + bcz * bcz;
            double const maxEdgeSquared = std::max({abSquared, acSquared, bcSquared});
            double const crossX = aby * acz - abz * acy;
            double const crossY = abz * acx - abx * acz;
            double const crossZ = abx * acy - aby * acx;
            double const crossSquared = crossX * crossX + crossY * crossY + crossZ * crossZ;
            if (maxEdgeSquared == 0.0 ||
                crossSquared <= maxEdgeSquared * maxEdgeSquared * relativeAreaTolerance)
            {
                ++result.degenerateTriangleCount;
            }

            double const ax = static_cast<double>(a.x()) - volumeOrigin.x();
            double const ay = static_cast<double>(a.y()) - volumeOrigin.y();
            double const az = static_cast<double>(a.z()) - volumeOrigin.z();
            double const bx = static_cast<double>(b.x()) - volumeOrigin.x();
            double const by = static_cast<double>(b.y()) - volumeOrigin.y();
            double const bz = static_cast<double>(b.z()) - volumeOrigin.z();
            double const cx = static_cast<double>(c.x()) - volumeOrigin.x();
            double const cy = static_cast<double>(c.y()) - volumeOrigin.y();
            double const cz = static_cast<double>(c.z()) - volumeOrigin.z();
            sixTimesSignedVolume += ax * (by * cz - bz * cy) +
                                    ay * (bz * cx - bx * cz) +
                                    az * (bx * cy - by * cx);

            for (std::size_t edge = 0U; edge < 3U; ++edge)
            {
                std::size_t const edgeStart = mesh.indices[offset + edge];
                std::size_t const edgeEnd = mesh.indices[offset + (edge + 1U) % 3U];
                std::size_t const minimum = std::min(edgeStart, edgeEnd);
                std::size_t const maximum = std::max(edgeStart, edgeEnd);
                std::uint64_t const key = (static_cast<std::uint64_t>(minimum) << 32U) |
                                          static_cast<std::uint64_t>(maximum);
                auto& use = edgeUses[key];
                ++use.faceCount;
                use.orientation += edgeStart < edgeEnd ? 1 : -1;
            }
        }
        result.signedVolume = sixTimesSignedVolume / 6.0;

        std::vector<std::size_t> componentRoots;
        componentRoots.reserve(mesh.positions.size());
        bool hasReferencedPosition = false;
        for (std::size_t vertex = 0U; vertex < referencedVertices.size(); ++vertex)
        {
            if (!referencedVertices[vertex])
            {
                continue;
            }

            Eigen::Vector3f const& position = mesh.positions[vertex];
            if (!hasReferencedPosition)
            {
                result.boundsMin = position;
                result.boundsMax = position;
                hasReferencedPosition = true;
            }
            else
            {
                result.boundsMin = result.boundsMin.cwiseMin(position);
                result.boundsMax = result.boundsMax.cwiseMax(position);
            }
            ++result.referencedVertexCount;
            componentRoots.push_back(findRoot(vertex));
        }
        std::sort(componentRoots.begin(), componentRoots.end());
        auto const uniqueRootEnd = std::unique(componentRoots.begin(), componentRoots.end());
        result.connectedComponentCount =
            static_cast<std::size_t>(uniqueRootEnd - componentRoots.begin());

        for (auto const& entry : edgeUses)
        {
            EdgeUse const& use = entry.second;
            if (use.faceCount == 1U)
            {
                ++result.boundaryEdgeCount;
            }
            else if (use.faceCount > 2U)
            {
                ++result.nonManifoldEdgeCount;
            }
            else if (use.orientation != 0)
            {
                ++result.inconsistentWindingEdgeCount;
            }
        }

        return result;
    }

    /// @brief CPU-owned indexed triangle mesh, independent of a compute backend.

    struct IndexedTriangleMesh
    {
        std::vector<Eigen::Vector3f> positions;
        std::vector<Eigen::Vector3f> normals;
        std::vector<std::uint32_t> indices;

        /// @brief Returns the first structural or geometric validation error, if any.
        [[nodiscard]] std::optional<std::string> validationError(
            IndexedTriangleMeshValidationPolicy policy = IndexedTriangleMeshValidationPolicy::Strict) const
        {
            if (positions.empty())
            {
                return "Mesh has no positions";
            }

            if (positions.size() > std::numeric_limits<std::uint32_t>::max())
            {
                return "Mesh exceeds the 32-bit vertex index limit";
            }

            if (indices.empty())
            {
                return "Mesh has no triangle indices";
            }

            if (indices.size() % 3U != 0U)
            {
                return "Mesh index count is not divisible by three";
            }

            if (!normals.empty() && normals.size() != positions.size())
            {
                return "Mesh normal count does not match its position count";
            }

            auto const isFinite = [](Eigen::Vector3f const & value)
            {
                return std::isfinite(value.x()) && std::isfinite(value.y()) &&
                       std::isfinite(value.z());
            };

            for (std::size_t i = 0; i < positions.size(); ++i)
            {
                if (!isFinite(positions[i]))
                {
                    return "Mesh contains a non-finite position";
                }
            }

            for (std::size_t i = 0; i < normals.size(); ++i)
            {
                if (!isFinite(normals[i]))
                {
                    return "Mesh contains a non-finite normal";
                }
            }

            double const floatEpsilon = std::numeric_limits<float>::epsilon();
            double const relativeAreaTolerance = floatEpsilon * floatEpsilon;
            for (std::size_t offset = 0; offset < indices.size(); offset += 3U)
            {
                auto const indexA = indices[offset];
                auto const indexB = indices[offset + 1U];
                auto const indexC = indices[offset + 2U];
                if (indexA >= positions.size() || indexB >= positions.size() ||
                    indexC >= positions.size())
                {
                    return "Mesh contains an out-of-range triangle index";
                }

                if (indexA == indexB || indexB == indexC || indexC == indexA)
                {
                    return "Mesh contains a triangle with repeated indices";
                }

                auto const & a = positions[indexA];
                auto const & b = positions[indexB];
                auto const & c = positions[indexC];
                double const abx = static_cast<double>(b.x()) - a.x();
                double const aby = static_cast<double>(b.y()) - a.y();
                double const abz = static_cast<double>(b.z()) - a.z();
                double const acx = static_cast<double>(c.x()) - a.x();
                double const acy = static_cast<double>(c.y()) - a.y();
                double const acz = static_cast<double>(c.z()) - a.z();
                double const bcx = static_cast<double>(c.x()) - b.x();
                double const bcy = static_cast<double>(c.y()) - b.y();
                double const bcz = static_cast<double>(c.z()) - b.z();

                double const abSquared = abx * abx + aby * aby + abz * abz;
                double const acSquared = acx * acx + acy * acy + acz * acz;
                double const bcSquared = bcx * bcx + bcy * bcy + bcz * bcz;
                double const maxEdgeSquared = std::max({abSquared, acSquared, bcSquared});

                double const crossX = aby * acz - abz * acy;
                double const crossY = abz * acx - abx * acz;
                double const crossZ = abx * acy - aby * acx;
                double const crossSquared = crossX * crossX + crossY * crossY + crossZ * crossZ;

                if (policy == IndexedTriangleMeshValidationPolicy::Strict &&
                    (maxEdgeSquared == 0.0 ||
                     crossSquared <= maxEdgeSquared * maxEdgeSquared * relativeAreaTolerance))
                {
                    return "Mesh contains a degenerate triangle";
                }
            }

            return std::nullopt;
        }

        /// @brief Returns a topology error unless every edge belongs to two oppositely wound faces.
        [[nodiscard]] std::optional<std::string> closedOrientedTopologyError(
            IndexedTriangleMeshValidationPolicy policy = IndexedTriangleMeshValidationPolicy::Strict) const
        {
            if (auto const error = validationError(policy))
            {
                return error;
            }

            struct EdgeUse
            {
                std::uint32_t faceCount{};
                std::int32_t orientation{};
            };

            std::unordered_map<std::uint64_t, EdgeUse> edgeUses;
            edgeUses.reserve(indices.size() / 2u);
            for (std::size_t offset = 0u; offset < indices.size(); offset += 3u)
            {
                for (std::size_t edge = 0u; edge < 3u; ++edge)
                {
                    auto const first = indices[offset + edge];
                    auto const second = indices[offset + (edge + 1u) % 3u];
                    auto const minimum = std::min(first, second);
                    auto const maximum = std::max(first, second);
                    auto const key = (static_cast<std::uint64_t>(minimum) << 32u) | maximum;
                    auto & use = edgeUses[key];
                    ++use.faceCount;
                    use.orientation += first < second ? 1 : -1;
                }
            }

            for (auto const & [key, use] : edgeUses)
            {
                (void)key;
                if (use.faceCount != 2u)
                {
                    return "Mesh is not closed: an edge does not have exactly two incident faces";
                }
                if (use.orientation != 0)
                {
                    return "Mesh has inconsistent face winding across a shared edge";
                }
            }

            return std::nullopt;
        }

        /// @brief Returns whether the mesh passes structural and geometric validation.
        [[nodiscard]] bool isValid() const
        {
            return !validationError().has_value();
        }
    };
} // namespace gladius::io