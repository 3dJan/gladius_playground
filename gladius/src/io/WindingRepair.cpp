#include "WindingRepair.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

namespace
{
    struct EdgeRef
    {
        std::uint32_t a{0U};
        std::uint32_t b{0U};
        std::uint32_t triId{0U};
        std::uint8_t dir{0U}; // 0: min->max, 1: max->min
    };

    [[nodiscard]] double computeSignedVolume(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<std::uint32_t> const& indices)
    {
        if (indices.size() < 3U)
        {
            return 0.0;
        }

        double volume6 = 0.0;
        for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
        {
            std::size_t const ia = static_cast<std::size_t>(indices[i + 0U]);
            std::size_t const ib = static_cast<std::size_t>(indices[i + 1U]);
            std::size_t const ic = static_cast<std::size_t>(indices[i + 2U]);
            if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size())
            {
                continue;
            }

            Eigen::Vector3d const a = positions[ia].cast<double>();
            Eigen::Vector3d const b = positions[ib].cast<double>();
            Eigen::Vector3d const c = positions[ic].cast<double>();
            volume6 += a.dot(b.cross(c));
        }
        return volume6 / 6.0;
    }
} // anonymous namespace

namespace gladius::io
{
    WindingRepairStats repairTriangleWindingConsistency(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<std::uint32_t>& indices)
    {
        WindingRepairStats stats;
        if (indices.size() < 3U || (indices.size() % 3U) != 0U)
        {
            return stats;
        }

        std::size_t const triCount = indices.size() / 3U;
        stats.triangleCount = triCount;

        std::vector<EdgeRef> edgeRefs;
        edgeRefs.reserve(triCount * 3U);

        for (std::size_t triId = 0U; triId < triCount; ++triId)
        {
            std::uint32_t const i0 = indices[triId * 3U + 0U];
            std::uint32_t const i1 = indices[triId * 3U + 1U];
            std::uint32_t const i2 = indices[triId * 3U + 2U];
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            auto pushEdge = [&edgeRefs, triId](std::uint32_t from, std::uint32_t to)
            {
                if (from == to)
                {
                    return;
                }

                std::uint32_t const lo = std::min(from, to);
                std::uint32_t const hi = std::max(from, to);
                std::uint8_t const dir = (from == lo) ? 0U : 1U;
                edgeRefs.push_back(EdgeRef{lo, hi, static_cast<std::uint32_t>(triId), dir});
            };

            pushEdge(i0, i1);
            pushEdge(i1, i2);
            pushEdge(i2, i0);
        }

        if (edgeRefs.empty())
        {
            return stats;
        }

        std::sort(edgeRefs.begin(),
                  edgeRefs.end(),
                  [](EdgeRef const& lhs, EdgeRef const& rhs)
                  {
                      if (lhs.a != rhs.a)
                      {
                          return lhs.a < rhs.a;
                      }
                      if (lhs.b != rhs.b)
                      {
                          return lhs.b < rhs.b;
                      }
                      return lhs.triId < rhs.triId;
                  });

        std::vector<std::array<std::uint32_t, 3>> neighbors(triCount, {
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max()});
        std::vector<std::array<std::uint8_t, 3>> neighborXor(triCount, {0U, 0U, 0U});
        std::vector<std::uint8_t> degree(triCount, 0U);

        auto addConstraint = [&neighbors, &neighborXor, &degree](std::uint32_t from,
                                                                 std::uint32_t to,
                                                                 std::uint8_t requiredXor)
        {
            std::uint8_t& d = degree[from];
            if (d >= 3U)
            {
                return;
            }
            neighbors[from][d] = to;
            neighborXor[from][d] = requiredXor;
            ++d;
        };

        // Build triangle adjacency with XOR constraints.
        std::size_t idx = 0U;
        while (idx < edgeRefs.size())
        {
            std::size_t const start = idx;
            std::uint32_t const a = edgeRefs[idx].a;
            std::uint32_t const b = edgeRefs[idx].b;
            while (idx < edgeRefs.size() && edgeRefs[idx].a == a && edgeRefs[idx].b == b)
            {
                ++idx;
            }
            std::size_t const count = idx - start;
            if (count != 2U)
            {
                continue;
            }

            EdgeRef const& e0 = edgeRefs[start + 0U];
            EdgeRef const& e1 = edgeRefs[start + 1U];
            if (e0.triId == e1.triId)
            {
                continue;
            }

            std::uint8_t const requiredXor = (e0.dir == e1.dir) ? 1U : 0U;
            addConstraint(e0.triId, e1.triId, requiredXor);
            addConstraint(e1.triId, e0.triId, requiredXor);
            ++stats.adjacencyConstraints;
        }

        // Assign flips per connected component.
        std::vector<std::int8_t> flip(triCount, static_cast<std::int8_t>(-1));
        std::queue<std::uint32_t> queue;

        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(triCount); ++t)
        {
            if (degree[t] == 0U || flip[t] != static_cast<std::int8_t>(-1))
            {
                continue;
            }

            ++stats.components;
            flip[t] = 0;
            queue.push(t);

            while (!queue.empty())
            {
                std::uint32_t const cur = queue.front();
                queue.pop();

                std::uint8_t const d = degree[cur];
                for (std::uint8_t n = 0U; n < d; ++n)
                {
                    std::uint32_t const other = neighbors[cur][n];
                    if (other == std::numeric_limits<std::uint32_t>::max())
                    {
                        continue;
                    }

                    std::uint8_t const requiredXor = neighborXor[cur][n];
                    std::int8_t const desired = static_cast<std::int8_t>(flip[cur] ^ requiredXor);
                    if (flip[other] == static_cast<std::int8_t>(-1))
                    {
                        flip[other] = desired;
                        queue.push(other);
                    }
                    else if (flip[other] != desired)
                    {
                        stats.hadInconsistency = true;
                    }
                }
            }
        }

        // Apply flips.
        for (std::size_t triId = 0U; triId < triCount; ++triId)
        {
            if (flip[triId] == 1)
            {
                std::swap(indices[triId * 3U + 1U], indices[triId * 3U + 2U]);
                ++stats.flippedTriangles;
            }
        }

        // Optional: enforce outward global orientation (positive signed volume) when possible.
        double const volume = computeSignedVolume(positions, indices);
        if (volume < 0.0)
        {
            for (std::size_t triId = 0U; triId < triCount; ++triId)
            {
                std::swap(indices[triId * 3U + 1U], indices[triId * 3U + 2U]);
            }
            stats.flippedGlobalOrientation = true;
        }

        return stats;
    }

} // namespace gladius::io
