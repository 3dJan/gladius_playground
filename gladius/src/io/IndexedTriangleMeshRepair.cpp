#include "io/IndexedTriangleMeshRepair.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace gladius::io
{
    namespace
    {
        constexpr std::size_t kMaximumRepairPasses = 32U;

        [[nodiscard]] double squaredEdgeLength(Eigen::Vector3f const& first, Eigen::Vector3f const& second)
        {
            Eigen::Vector3d const delta = (second - first).cast<double>();
            return delta.squaredNorm();
        }

        /// @brief Resolves collapse targets, keeping the endpoint with the larger valence.
        struct CollapseMap
        {
            std::vector<std::uint32_t> parent;

            explicit CollapseMap(std::size_t vertexCount)
                : parent(vertexCount)
            {
                for (std::size_t vertex = 0U; vertex < vertexCount; ++vertex)
                {
                    parent[vertex] = static_cast<std::uint32_t>(vertex);
                }
            }

            [[nodiscard]] std::uint32_t findRoot(std::uint32_t vertex)
            {
                std::uint32_t root = vertex;
                while (parent[root] != root)
                {
                    root = parent[root];
                }
                while (parent[vertex] != root)
                {
                    std::uint32_t const next = parent[vertex];
                    parent[vertex] = root;
                    vertex = next;
                }
                return root;
            }

            void mergeInto(std::uint32_t kept, std::uint32_t removed)
            {
                parent[findRoot(removed)] = findRoot(kept);
            }
        };

        /// @brief Returns the vertices that share a triangle with @p vertex, resolved through @p map.
        [[nodiscard]] std::vector<std::uint32_t> collectNeighbours(std::uint32_t vertex,
                                                                  std::vector<std::uint32_t> const& triangles,
                                                                  std::vector<std::uint32_t> const& indices,
                                                                  CollapseMap& map)
        {
            std::vector<std::uint32_t> neighbours;
            auto const root = map.findRoot(vertex);
            for (auto const triangle : triangles)
            {
                for (std::size_t corner = 0U; corner < 3U; ++corner)
                {
                    auto const neighbour = map.findRoot(indices[triangle * 3U + corner]);
                    if (neighbour == root)
                    {
                        continue;
                    }
                    if (std::find(neighbours.begin(), neighbours.end(), neighbour) == neighbours.end())
                    {
                        neighbours.push_back(neighbour);
                    }
                }
            }
            std::sort(neighbours.begin(), neighbours.end());
            return neighbours;
        }

        /// @brief Returns the triangles incident to both endpoints, resolved through @p map.
        [[nodiscard]] std::vector<std::uint32_t> collectSharedTriangles(std::uint32_t first,
                                                                       std::uint32_t second,
                                                                       std::vector<std::vector<std::uint32_t>> const& vertexTriangles)
        {
            auto const& firstTriangles = vertexTriangles[first];
            auto const& secondTriangles = vertexTriangles[second];
            std::vector<std::uint32_t> shared;
            shared.reserve(std::min(firstTriangles.size(), secondTriangles.size()));
            std::set_intersection(firstTriangles.begin(), firstTriangles.end(),
                                  secondTriangles.begin(), secondTriangles.end(),
                                  std::back_inserter(shared));
            return shared;
        }

        /// @brief An edge collapse keeps a closed 2-manifold only when the endpoints share exactly two
        ///        neighbours and exactly two faces, i.e. the two faces that fold away are the only
        ///        faces between them. Both requirements are needed: on a non-manifold edge the link
        ///        can still hold while a third face keeps the collapsed edge as a boundary.
        [[nodiscard]] bool isLinkSafeCollapse(std::uint32_t first,
                                              std::uint32_t second,
                                              std::vector<std::vector<std::uint32_t>> const& vertexTriangles,
                                              std::vector<std::uint32_t> const& indices,
                                              CollapseMap& map)
        {
            auto const firstRoot = map.findRoot(first);
            auto const secondRoot = map.findRoot(second);
            if (vertexTriangles[firstRoot].empty() || vertexTriangles[secondRoot].empty())
            {
                return false;
            }
            if (collectSharedTriangles(firstRoot, secondRoot, vertexTriangles).size() != 2U)
            {
                return false;
            }

            auto firstNeighbours = collectNeighbours(firstRoot, vertexTriangles[firstRoot], indices, map);
            auto secondNeighbours = collectNeighbours(secondRoot, vertexTriangles[secondRoot], indices, map);
            std::vector<std::uint32_t> sharedNeighbours;
            std::set_intersection(firstNeighbours.begin(), firstNeighbours.end(),
                                  secondNeighbours.begin(), secondNeighbours.end(),
                                  std::back_inserter(sharedNeighbours));
            return sharedNeighbours.size() == 2U;
        }

        /// @brief Counts triangles that repeat a vertex index.
        [[nodiscard]] std::size_t countTrianglesWithRepeatedIndices(IndexedTriangleMesh const& mesh)
        {
            std::size_t count = 0U;
            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                if (mesh.indices[offset] == mesh.indices[offset + 1U] ||
                    mesh.indices[offset + 1U] == mesh.indices[offset + 2U] ||
                    mesh.indices[offset + 2U] == mesh.indices[offset])
                {
                    ++count;
                }
            }
            return count;
        }

        /// @brief Drops triangles that lost distinct corners and removes unreferenced vertices.
        /// @param triangleLabels Optional per-triangle labels filtered with the same predicate.
        void compactMesh(IndexedTriangleMesh& mesh,
                         std::vector<std::uint32_t>* triangleLabels)
        {
            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(mesh.indices.size());
            std::vector<std::uint32_t> newLabels;
            if (triangleLabels != nullptr)
            {
                newLabels.reserve(triangleLabels->size());
            }
            std::vector<std::uint32_t> remap(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());
            std::uint32_t nextVertex = 0U;

            auto const resolve = [&remap, &nextVertex, &mesh](std::uint32_t vertex)
            {
                if (remap[vertex] == std::numeric_limits<std::uint32_t>::max())
                {
                    remap[vertex] = nextVertex++;
                }
                return remap[vertex];
            };

            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                auto const first = mesh.indices[offset];
                auto const second = mesh.indices[offset + 1U];
                auto const third = mesh.indices[offset + 2U];
                if (first == second || second == third || third == first)
                {
                    continue;
                }
                newIndices.push_back(resolve(first));
                newIndices.push_back(resolve(second));
                newIndices.push_back(resolve(third));
                if (triangleLabels != nullptr)
                {
                    newLabels.push_back((*triangleLabels)[offset / 3U]);
                }
            }

            std::vector<Eigen::Vector3f> positions;
            positions.reserve(nextVertex);
            for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex)
            {
                if (remap[vertex] != std::numeric_limits<std::uint32_t>::max())
                {
                    positions.push_back(mesh.positions[vertex]);
                }
            }

            std::vector<Eigen::Vector3f> normals;
            if (mesh.normals.size() == mesh.positions.size())
            {
                normals.reserve(nextVertex);
                for (std::size_t vertex = 0U; vertex < mesh.normals.size(); ++vertex)
                {
                    if (remap[vertex] != std::numeric_limits<std::uint32_t>::max())
                    {
                        normals.push_back(mesh.normals[vertex]);
                    }
                }
            }

            mesh.indices.swap(newIndices);
            mesh.positions.swap(positions);
            mesh.normals.swap(normals);
            if (triangleLabels != nullptr)
            {
                triangleLabels->swap(newLabels);
            }
        }
    }

    std::size_t countGeometricallyDegenerateTriangles(IndexedTriangleMesh const& mesh)
    {
        std::size_t count = 0U;
        for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
        {
            if (isGeometricallyDegenerateTriangle(mesh.positions[mesh.indices[offset]],
                                                 mesh.positions[mesh.indices[offset + 1U]],
                                                 mesh.positions[mesh.indices[offset + 2U]]))
            {
                ++count;
            }
        }
        return count;
    }

    IndexedTriangleMeshRepairResult collapseGeometricallyDegenerateTriangles(IndexedTriangleMesh& mesh)
    {
        std::vector<std::uint32_t> unusedLabels;
        return collapseGeometricallyDegenerateTriangles(mesh, unusedLabels);
    }

    IndexedTriangleMeshRepairResult collapseGeometricallyDegenerateTriangles(
        IndexedTriangleMesh& mesh,
        std::vector<std::uint32_t>& triangleLabels)
    {
        IndexedTriangleMeshRepairResult result;
        // Labels are only meaningful while they line up one-to-one with the triangles.
        std::vector<std::uint32_t>* labels =
            triangleLabels.size() == mesh.indices.size() / 3ULL ? &triangleLabels : nullptr;
        if (mesh.indices.size() % 3U != 0U || mesh.positions.empty())
        {
            result.remainingDegenerateTriangleCount = countGeometricallyDegenerateTriangles(mesh);
            return result;
        }

        // A triangle that already repeats a vertex index contributes no area and cannot be collapsed
        // (there is no edge to fold), so it is dropped up front. Grid extractors can emit such faces
        // when a grid sample sits exactly on the isosurface.
        if (auto const repeated = countTrianglesWithRepeatedIndices(mesh); repeated != 0U)
        {
            result.removedTriangleCount += repeated;
            compactMesh(mesh, labels);
        }

        for (std::size_t pass = 0U; pass < kMaximumRepairPasses; ++pass)
        {
            std::vector<std::vector<std::uint32_t>> vertexTriangles(mesh.positions.size());
            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                for (std::size_t corner = 0U; corner < 3U; ++corner)
                {
                    auto const vertex = mesh.indices[offset + corner];
                    if (vertex < vertexTriangles.size())
                    {
                        vertexTriangles[vertex].push_back(static_cast<std::uint32_t>(offset / 3U));
                    }
                }
            }

            std::vector<std::uint32_t> degenerateTriangles;
            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                if (isGeometricallyDegenerateTriangle(mesh.positions[mesh.indices[offset]],
                                                     mesh.positions[mesh.indices[offset + 1U]],
                                                     mesh.positions[mesh.indices[offset + 2U]]))
                {
                    degenerateTriangles.push_back(static_cast<std::uint32_t>(offset / 3U));
                }
            }
            if (degenerateTriangles.empty())
            {
                break;
            }

            CollapseMap collapseMap{mesh.positions.size()};
            std::size_t collapsedInPass = 0U;
            for (auto const triangle : degenerateTriangles)
            {
                auto const offset = static_cast<std::size_t>(triangle) * 3U;
                std::array<std::uint32_t, 3U> corners{mesh.indices[offset],
                                                      mesh.indices[offset + 1U],
                                                      mesh.indices[offset + 2U]};

                std::array<std::size_t, 3U> edges{0U, 1U, 2U};
                std::sort(edges.begin(), edges.end(),
                          [&corners, &collapseMap, &mesh](std::size_t left, std::size_t right)
                          {
                              auto const leftLength = squaredEdgeLength(
                                mesh.positions[collapseMap.findRoot(corners[left])],
                                mesh.positions[collapseMap.findRoot(corners[(left + 1U) % 3U])]);
                              auto const rightLength = squaredEdgeLength(
                                mesh.positions[collapseMap.findRoot(corners[right])],
                                mesh.positions[collapseMap.findRoot(corners[(right + 1U) % 3U])]);
                              return leftLength < rightLength;
                          });

                bool collapsed = false;
                for (auto const edge : edges)
                {
                    auto const first = collapseMap.findRoot(corners[edge]);
                    auto const second = collapseMap.findRoot(corners[(edge + 1U) % 3U]);
                    if (first == second ||
                        !isLinkSafeCollapse(first, second, vertexTriangles, mesh.indices, collapseMap))
                    {
                        continue;
                    }

                    // Keep the vertex with the larger valence so the merged ring stays as regular as possible.
                    auto const keep =
                        vertexTriangles[first].size() >= vertexTriangles[second].size() ? first : second;
                    auto const drop = keep == first ? second : first;
                    collapseMap.mergeInto(keep, drop);

                    // Fold the dropped vertex's incidence into the kept one so later collapses in this
                    // pass see the merged ring instead of a stale one.
                    auto& keptTriangles = vertexTriangles[keep];
                    auto& droppedTriangles = vertexTriangles[drop];
                    keptTriangles.insert(keptTriangles.end(),
                                         std::make_move_iterator(droppedTriangles.begin()),
                                         std::make_move_iterator(droppedTriangles.end()));
                    droppedTriangles.clear();
                    std::ranges::sort(keptTriangles);
                    keptTriangles.erase(std::unique(keptTriangles.begin(), keptTriangles.end()),
                                        keptTriangles.end());

                    collapsed = true;
                    ++collapsedInPass;
                    ++result.collapsedEdgeCount;
                    break;
                }
                (void)collapsed;
            }

            if (collapsedInPass == 0U)
            {
                break;
            }

            std::size_t foldedTriangles = 0U;
            for (auto& index : mesh.indices)
            {
                index = collapseMap.findRoot(index);
            }
            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                if (mesh.indices[offset] == mesh.indices[offset + 1U] ||
                    mesh.indices[offset + 1U] == mesh.indices[offset + 2U] ||
                    mesh.indices[offset + 2U] == mesh.indices[offset])
                {
                    ++foldedTriangles;
                }
            }
            result.removedTriangleCount += foldedTriangles;

            auto const vertexCountBefore = mesh.positions.size();
            compactMesh(mesh, labels);
            result.removedVertexCount += vertexCountBefore - mesh.positions.size();
        }

        result.remainingDegenerateTriangleCount = countGeometricallyDegenerateTriangles(mesh);
        return result;
    }
}
