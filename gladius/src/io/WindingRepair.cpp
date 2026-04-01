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

    /// Pack two uint32 vertex indices into a single uint64 edge key.
    [[nodiscard]] std::uint64_t makeEdgeKey(std::uint32_t a, std::uint32_t b)
    {
        std::uint32_t const lo = std::min(a, b);
        std::uint32_t const hi = std::max(a, b);
        return (static_cast<std::uint64_t>(lo) << 32U) | hi;
    }

    /// High-quality hash for uint64 edge keys (splitmix64 finalizer).
    /// std::hash<uint64_t> is often the identity on libstdc++, which causes
    /// massive clustering with power-of-two table sizes.
    [[nodiscard]] std::size_t hashEdgeKey(std::uint64_t key)
    {
        key ^= key >> 30U;
        key *= 0xbf58476d1ce4e5b9ULL;
        key ^= key >> 27U;
        key *= 0x94d049bb133111ebULL;
        key ^= key >> 31U;
        return static_cast<std::size_t>(key);
    }

    struct EdgeSlot
    {
        static constexpr std::uint64_t EMPTY_KEY = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t key = EMPTY_KEY;
        std::uint32_t triId = 0U;
        std::uint8_t dir = 0U;
        bool matched = false;
    };
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

        // Build triangle adjacency using a flat open-addressing hash table.
        // This reduces complexity from O(N log N) (sort-based) to amortized O(N)
        // with cache-friendly linear probing and zero per-element heap allocations.

        // Size the table to the next power of 2 >= 2 * expected unique edges.
        // For a manifold mesh: unique edges ≈ triCount * 3 / 2.
        std::size_t tableSize = 1U;
        std::size_t const targetSize = std::max<std::size_t>(triCount * 3U, 64U);
        while (tableSize < targetSize)
        {
            tableSize <<= 1U;
        }

        std::vector<EdgeSlot> hashTable(tableSize);

        auto findSlot = [&hashTable, tableSize](std::uint64_t key) -> std::size_t
        {
            std::size_t idx = hashEdgeKey(key) & (tableSize - 1U);
            while (hashTable[idx].key != EdgeSlot::EMPTY_KEY && hashTable[idx].key != key)
            {
                idx = (idx + 1U) & (tableSize - 1U);
            }
            return idx;
        };

        for (std::size_t triId = 0U; triId < triCount; ++triId)
        {
            std::uint32_t const i0 = indices[triId * 3U + 0U];
            std::uint32_t const i1 = indices[triId * 3U + 1U];
            std::uint32_t const i2 = indices[triId * 3U + 2U];
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                continue;
            }

            auto processEdge = [&](std::uint32_t from, std::uint32_t to)
            {
                if (from == to)
                {
                    return;
                }

                std::uint64_t const key = makeEdgeKey(from, to);
                std::uint8_t const dir = (from <= to) ? 0U : 1U;

                std::size_t const idx = findSlot(key);
                EdgeSlot& slot = hashTable[idx];

                if (slot.key == EdgeSlot::EMPTY_KEY)
                {
                    // First time seeing this edge — insert.
                    slot.key = key;
                    slot.triId = static_cast<std::uint32_t>(triId);
                    slot.dir = dir;
                }
                else if (!slot.matched)
                {
                    // Second occurrence — record adjacency constraint.
                    if (slot.triId != static_cast<std::uint32_t>(triId))
                    {
                        std::uint8_t const requiredXor = (slot.dir == dir) ? 1U : 0U;
                        addConstraint(slot.triId, static_cast<std::uint32_t>(triId), requiredXor);
                        addConstraint(static_cast<std::uint32_t>(triId), slot.triId, requiredXor);
                        ++stats.adjacencyConstraints;
                    }
                    slot.matched = true;
                }
                // Third+ occurrence (non-manifold edge): silently ignored.
            };

            processEdge(i0, i1);
            processEdge(i1, i2);
            processEdge(i2, i0);
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
