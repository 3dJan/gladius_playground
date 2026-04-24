/// @file MeshRepair.cpp
/// @brief Implementation of host-side mesh cleanup operations.
/// @see MeshRepair.h

#include "MeshRepair.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gladius::mesh_repair
{
    namespace
    {
        /// Quantise a coordinate into a fixed-precision integer bucket.
        /// Two coordinates are placed into the same bucket iff they round to
        /// the same multiple of @p epsilon (roughly).
        inline std::int64_t quantise(float v, float epsilon)
        {
            return static_cast<std::int64_t>(std::llround(static_cast<double>(v) /
                                                          static_cast<double>(epsilon)));
        }

        struct GridKey
        {
            std::int64_t x;
            std::int64_t y;
            std::int64_t z;
            bool operator==(GridKey const & o) const noexcept
            {
                return x == o.x && y == o.y && z == o.z;
            }
        };

        struct GridKeyHash
        {
            std::size_t operator()(GridKey const & k) const noexcept
            {
                // Mix three int64 into a single hash (Boost-style combine).
                auto const h1 = std::hash<std::int64_t>{}(k.x);
                auto const h2 = std::hash<std::int64_t>{}(k.y);
                auto const h3 = std::hash<std::int64_t>{}(k.z);
                std::size_t seed = h1;
                seed ^= h2 + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                seed ^= h3 + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        inline float triangleArea(float4 const & a, float4 const & b, float4 const & c)
        {
            float const ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            float const vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            float const cx = uy * vz - uz * vy;
            float const cy = uz * vx - ux * vz;
            float const cz = ux * vy - uy * vx;
            return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
        }

        /// Undirected edge key; orders endpoints so (a,b) and (b,a) hash equal.
        struct EdgeKey
        {
            std::uint64_t key;
            bool operator==(EdgeKey const & o) const noexcept { return key == o.key; }
        };

        struct EdgeKeyHash
        {
            std::size_t operator()(EdgeKey const & k) const noexcept
            {
                return std::hash<std::uint64_t>{}(k.key);
            }
        };

        inline EdgeKey makeEdgeKey(int a, int b)
        {
            std::uint32_t const lo = static_cast<std::uint32_t>(std::min(a, b));
            std::uint32_t const hi = static_cast<std::uint32_t>(std::max(a, b));
            return EdgeKey{(static_cast<std::uint64_t>(hi) << 32) |
                           static_cast<std::uint64_t>(lo)};
        }
    } // namespace

    // ========================================================================
    // weldVertices
    // ========================================================================

    std::size_t weldVertices(std::vector<float4> & vertices,
                             std::vector<TriangleIndices> & indices,
                             float epsilon)
    {
        if (vertices.empty() || epsilon <= 0.f)
        {
            return 0u;
        }

        // Map each input vertex index to a representative output vertex.
        std::vector<int> remap(vertices.size(), -1);
        std::unordered_map<GridKey, int, GridKeyHash> bucket;
        bucket.reserve(vertices.size());

        std::vector<float4> outVertices;
        outVertices.reserve(vertices.size());

        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            float4 const & v = vertices[i];
            GridKey const key{quantise(v.x, epsilon),
                              quantise(v.y, epsilon),
                              quantise(v.z, epsilon)};
            auto const it = bucket.find(key);
            if (it == bucket.end())
            {
                int const newIdx = static_cast<int>(outVertices.size());
                bucket.emplace(key, newIdx);
                remap[i] = newIdx;
                outVertices.push_back(v);
            }
            else
            {
                remap[i] = it->second;
            }
        }

        std::size_t const removed = vertices.size() - outVertices.size();
        if (removed == 0u)
        {
            return 0u;
        }

        // Remap triangle indices.
        for (auto & tri : indices)
        {
            tri.i0 = remap[static_cast<std::size_t>(tri.i0)];
            tri.i1 = remap[static_cast<std::size_t>(tri.i1)];
            tri.i2 = remap[static_cast<std::size_t>(tri.i2)];
        }

        vertices = std::move(outVertices);
        return removed;
    }

    // ========================================================================
    // removeDegenerateTriangles
    // ========================================================================

    std::size_t removeDegenerateTriangles(std::vector<float4> const & vertices,
                                          std::vector<TriangleIndices> & indices,
                                          float areaEpsilon)
    {
        std::size_t const before = indices.size();
        auto const newEnd = std::remove_if(
            indices.begin(),
            indices.end(),
            [&](TriangleIndices const & t)
            {
                if (t.i0 == t.i1 || t.i1 == t.i2 || t.i0 == t.i2)
                {
                    return true; // collapsed indices
                }
                if (static_cast<std::size_t>(t.i0) >= vertices.size() ||
                    static_cast<std::size_t>(t.i1) >= vertices.size() ||
                    static_cast<std::size_t>(t.i2) >= vertices.size())
                {
                    return true; // out-of-range index
                }
                float const area = triangleArea(vertices[static_cast<std::size_t>(t.i0)],
                                                vertices[static_cast<std::size_t>(t.i1)],
                                                vertices[static_cast<std::size_t>(t.i2)]);
                return area < areaEpsilon;
            });
        indices.erase(newEnd, indices.end());
        return before - indices.size();
    }

    // ========================================================================
    // orientConsistently
    // ========================================================================

    std::size_t orientConsistently(std::vector<float4> const & vertices,
                                   std::vector<TriangleIndices> & indices)
    {
        if (indices.empty())
        {
            return 0u;
        }
        std::size_t const triCount = indices.size();

        // Build directed-edge → triangle map. A consistently oriented manifold
        // produces opposing directed edges between any pair of neighbouring
        // triangles; matching directions indicates a needed flip.
        struct DirectedEdgeKey
        {
            std::uint64_t key;
            bool operator==(DirectedEdgeKey const & o) const noexcept { return key == o.key; }
        };
        struct DirectedEdgeKeyHash
        {
            std::size_t operator()(DirectedEdgeKey const & k) const noexcept
            {
                return std::hash<std::uint64_t>{}(k.key);
            }
        };
        auto const dirKey = [](int a, int b)
        {
            return DirectedEdgeKey{
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(b))};
        };

        // Adjacency: for each triangle, list of (neighbourTri, sameDirection?).
        std::vector<std::vector<std::pair<int, bool>>> adj(triCount);

        // First pass: register all directed edges grouped by undirected key.
        std::unordered_map<EdgeKey, std::vector<std::pair<int, std::pair<int, int>>>, EdgeKeyHash>
            undirected;
        undirected.reserve(triCount * 3u);

        auto const addEdge = [&](int triIdx, int a, int b)
        {
            undirected[makeEdgeKey(a, b)].push_back({triIdx, {a, b}});
        };
        for (std::size_t i = 0; i < triCount; ++i)
        {
            auto const & t = indices[i];
            addEdge(static_cast<int>(i), t.i0, t.i1);
            addEdge(static_cast<int>(i), t.i1, t.i2);
            addEdge(static_cast<int>(i), t.i2, t.i0);
        }

        // For each undirected edge with exactly two incident triangles, build
        // the adjacency entry. If their directed edges agree (a→b on both),
        // one of them must be flipped relative to the other.
        for (auto const & [_, refs] : undirected)
        {
            (void) _;
            if (refs.size() != 2u)
            {
                continue;
            }
            int const t0 = refs[0].first;
            int const t1 = refs[1].first;
            bool const sameDirection = (refs[0].second.first == refs[1].second.first &&
                                        refs[0].second.second == refs[1].second.second);
            adj[static_cast<std::size_t>(t0)].push_back({t1, sameDirection});
            adj[static_cast<std::size_t>(t1)].push_back({t0, sameDirection});
        }

        // BFS per connected component. Within each component, propagate a
        // "shouldFlipRelativeToSeed" flag; same-direction adjacency means
        // the two faces disagree → flip flag toggles across that edge.
        std::vector<int> componentOf(triCount, -1);
        std::vector<bool> flipRelative(triCount, false);
        std::vector<std::vector<int>> components;

        for (std::size_t seed = 0; seed < triCount; ++seed)
        {
            if (componentOf[seed] != -1)
            {
                continue;
            }
            int const compId = static_cast<int>(components.size());
            components.emplace_back();
            std::queue<int> q;
            q.push(static_cast<int>(seed));
            componentOf[seed] = compId;
            while (!q.empty())
            {
                int const cur = q.front();
                q.pop();
                components[static_cast<std::size_t>(compId)].push_back(cur);
                for (auto const & [nb, sameDir] : adj[static_cast<std::size_t>(cur)])
                {
                    if (componentOf[static_cast<std::size_t>(nb)] != -1)
                    {
                        continue;
                    }
                    componentOf[static_cast<std::size_t>(nb)] = compId;
                    flipRelative[static_cast<std::size_t>(nb)] =
                        flipRelative[static_cast<std::size_t>(cur)] ^ sameDir;
                    q.push(nb);
                }
            }
        }

        // Per-component majority area vote: sum the areas of triangles whose
        // current orientation matches the seed (flipRelative=false) versus those
        // that would be flipped. Whichever group has more area wins.
        std::size_t flipCount = 0u;
        for (auto const & comp : components)
        {
            float keepArea = 0.f;
            float flipArea = 0.f;
            for (int triIdx : comp)
            {
                auto const & t = indices[static_cast<std::size_t>(triIdx)];
                float const a = triangleArea(vertices[static_cast<std::size_t>(t.i0)],
                                             vertices[static_cast<std::size_t>(t.i1)],
                                             vertices[static_cast<std::size_t>(t.i2)]);
                if (flipRelative[static_cast<std::size_t>(triIdx)])
                {
                    flipArea += a;
                }
                else
                {
                    keepArea += a;
                }
            }
            // If "would-be-flipped" group has greater area, that's the dominant
            // orientation — invert the flip flag for the whole component.
            bool const invertComponent = (flipArea > keepArea);
            for (int triIdx : comp)
            {
                bool const finalFlip =
                    flipRelative[static_cast<std::size_t>(triIdx)] ^ invertComponent;
                if (finalFlip)
                {
                    auto & t = indices[static_cast<std::size_t>(triIdx)];
                    std::swap(t.i1, t.i2);
                    ++flipCount;
                }
            }
        }
        return flipCount;
    }

    // ========================================================================
    // fillSmallHoles
    // ========================================================================

    void fillSmallHoles(std::vector<float4> & vertices,
                        std::vector<TriangleIndices> & indices,
                        float maxPerimeter,
                        std::size_t & outFilled,
                        std::size_t & outAdded)
    {
        outFilled = 0u;
        outAdded = 0u;
        if (indices.empty())
        {
            return;
        }

        // Collect boundary directed edges: any directed edge (a,b) for which
        // the opposing (b,a) does not exist. Build a successor map next[a] = b
        // along the boundary so we can walk loops.
        std::unordered_set<std::uint64_t> directedEdges;
        directedEdges.reserve(indices.size() * 3u);
        auto const dirHash = [](int a, int b)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(b));
        };
        for (auto const & t : indices)
        {
            directedEdges.insert(dirHash(t.i0, t.i1));
            directedEdges.insert(dirHash(t.i1, t.i2));
            directedEdges.insert(dirHash(t.i2, t.i0));
        }

        std::unordered_map<int, int> boundaryNext;
        boundaryNext.reserve(64u);
        for (auto const & t : indices)
        {
            auto const tryAdd = [&](int a, int b)
            {
                if (directedEdges.find(dirHash(b, a)) == directedEdges.end())
                {
                    boundaryNext.emplace(a, b);
                }
            };
            tryAdd(t.i0, t.i1);
            tryAdd(t.i1, t.i2);
            tryAdd(t.i2, t.i0);
        }

        // Walk closed loops along boundaryNext.
        std::unordered_set<int> visited;
        visited.reserve(boundaryNext.size());

        for (auto const & [start, _] : boundaryNext)
        {
            (void) _;
            if (visited.count(start) != 0u)
            {
                continue;
            }
            // Trace loop
            std::vector<int> loop;
            loop.reserve(8u);
            int cur = start;
            bool closed = false;
            while (true)
            {
                if (visited.count(cur) != 0u)
                {
                    closed = (cur == start) && !loop.empty();
                    break;
                }
                visited.insert(cur);
                loop.push_back(cur);
                auto const it = boundaryNext.find(cur);
                if (it == boundaryNext.end())
                {
                    break;
                }
                cur = it->second;
                if (cur == start)
                {
                    closed = true;
                    break;
                }
                if (loop.size() > boundaryNext.size())
                {
                    break; // safety
                }
            }
            if (!closed || loop.size() < 3u)
            {
                continue;
            }

            // Compute perimeter and centroid.
            float perimeter = 0.f;
            float cx = 0.f, cy = 0.f, cz = 0.f;
            for (std::size_t i = 0; i < loop.size(); ++i)
            {
                float4 const & a = vertices[static_cast<std::size_t>(loop[i])];
                float4 const & b = vertices[static_cast<std::size_t>(loop[(i + 1u) % loop.size()])];
                float const dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                perimeter += std::sqrt(dx * dx + dy * dy + dz * dz);
                cx += a.x;
                cy += a.y;
                cz += a.z;
            }
            if (perimeter > maxPerimeter)
            {
                continue; // intentional opening, leave alone
            }
            float const inv = 1.f / static_cast<float>(loop.size());
            cx *= inv;
            cy *= inv;
            cz *= inv;

            // Add centroid vertex and fan-triangulate. The boundary walk follows
            // the directed edge (a→b) that the surrounding mesh has left
            // unmatched. To close it, the new fan triangle must contribute the
            // OPPOSITE directed edge (b→a), so we wind the fan as
            // (a, centroid, b) — its edges are a→centroid, centroid→b, b→a.
            int const centroidIdx = static_cast<int>(vertices.size());
            vertices.push_back(float4{cx, cy, cz, 0.f});
            for (std::size_t i = 0; i < loop.size(); ++i)
            {
                int const a = loop[i];
                int const b = loop[(i + 1u) % loop.size()];
                indices.push_back(TriangleIndices{a, centroidIdx, b});
            }
            outAdded += loop.size();
            ++outFilled;
        }
    }

    // ========================================================================
    // repairMesh orchestrator
    // ========================================================================

    MeshRepairResult repairMesh(std::vector<float4> & vertices,
                                std::vector<TriangleIndices> & indices,
                                MeshRepairConfig const & config)
    {
        MeshRepairResult result{};

        if (config.weld)
        {
            result.weldedVertices = weldVertices(vertices, indices, config.weldEpsilon);
        }
        if (config.removeDegenerate)
        {
            result.removedTriangles =
                removeDegenerateTriangles(vertices, indices, config.areaEpsilon);
        }
        if (config.orientConsistently)
        {
            result.flippedTriangles = orientConsistently(vertices, indices);
        }
        if (config.fillHoles)
        {
            fillSmallHoles(vertices,
                           indices,
                           config.maxHolePerimeter,
                           result.filledHoles,
                           result.addedTriangles);
        }
        return result;
    }

} // namespace gladius::mesh_repair
