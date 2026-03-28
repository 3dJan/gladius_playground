#include "FastQemSimplification.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gladius::compute
{
    namespace
    {
        /// Per-triangle metadata
        struct TriInfo
        {
            Eigen::Vector3f normal{0.F, 0.F, 0.F};
            std::uint8_t minEdgeIdx{0U};
            bool deleted{false};
        };

        /// Priority queue element — one per non-deleted triangle
        struct TriError
        {
            float value{std::numeric_limits<float>::max()};
            std::uint32_t triIdx{};
            std::size_t heapIdx{std::numeric_limits<std::size_t>::max()};
        };

        Eigen::Vector3f calcNormal(Eigen::Vector3f const & v0,
                                   Eigen::Vector3f const & v1,
                                   Eigen::Vector3f const & v2)
        {
            Eigen::Vector3f n = (v1 - v0).cross(v2 - v0);
            float const len = n.norm();
            if (len < 1e-20F)
            {
                return {0.F, 0.F, 0.F};
            }
            return n / len;
        }

        /// Compute error for collapsing edge (vi, vj), and find optimal position
        float edgeError(SymMat const & qi,
                        SymMat const & qj,
                        Eigen::Vector3f const & pi,
                        Eigen::Vector3f const & pj,
                        Eigen::Vector3f & outPos)
        {
            SymMat const combined = qi + qj;

            Eigen::Vector3f const mid = (pi + pj) * 0.5F;
            float const edgeLen = (pi - pj).norm();

            auto opt = combined.optimalVertex();
            if (opt.has_value())
            {
                // Sanity check: if the optimal position is farther from the edge
                // midpoint than the edge length, the quadric is ill-conditioned
                // and the result is unreliable.  Fall through to the endpoint/
                // midpoint fallback instead.
                float const dist = (*opt - mid).norm();
                if (dist <= edgeLen)
                {
                    outPos = *opt;
                    return static_cast<float>(combined.evaluate(outPos));
                }
            }

            // Fallback: best of both endpoints and midpoint
            float const errI = static_cast<float>(combined.evaluate(pi));
            float const errJ = static_cast<float>(combined.evaluate(pj));
            float const errM = static_cast<float>(combined.evaluate(mid));

            if (errI <= errJ && errI <= errM)
            {
                outPos = pi;
                return errI;
            }
            if (errJ <= errI && errJ <= errM)
            {
                outPos = pj;
                return errJ;
            }
            outPos = mid;
            return errM;
        }

        /// Compute the minimum-error edge for a triangle and store minEdgeIdx
        float evalTriMinEdge(std::uint32_t t,
                             std::vector<std::uint32_t> const & indices,
                             std::vector<Eigen::Vector3f> const & positions,
                             std::vector<SymMat> const & quadrics,
                             std::vector<TriInfo> & tris)
        {
            auto const i0 = indices[t * 3U + 0U];
            auto const i1 = indices[t * 3U + 1U];
            auto const i2 = indices[t * 3U + 2U];

            Eigen::Vector3f dummy;
            float const err0 = edgeError(quadrics[i0], quadrics[i1],
                                         positions[i0], positions[i1], dummy);
            float const err1 = edgeError(quadrics[i1], quadrics[i2],
                                         positions[i1], positions[i2], dummy);
            float const err2 = edgeError(quadrics[i2], quadrics[i0],
                                         positions[i2], positions[i0], dummy);

            float minErr = err0;
            std::uint8_t minIdx = 0U;
            if (err1 < minErr) { minErr = err1; minIdx = 1U; }
            if (err2 < minErr) { minErr = err2; minIdx = 2U; }
            tris[t].minEdgeIdx = minIdx;
            return minErr;
        }

        /// Get two vertex indices for edge within a triangle
        std::pair<std::uint32_t, std::uint32_t> triEdgeVerts(
            std::uint32_t t, std::uint8_t e, std::vector<std::uint32_t> const & indices)
        {
            constexpr std::uint8_t E[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            return {indices[t * 3U + E[e][0]], indices[t * 3U + E[e][1]]};
        }

        /// Check if moving id0 to newPos would flip any surrounding triangle
        bool wouldFlip(std::uint32_t id0,
                       std::uint32_t id1,
                       Eigen::Vector3f const & newPos,
                       std::vector<Eigen::Vector3f> const & positions,
                       std::vector<std::uint32_t> const & indices,
                       std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                       std::vector<TriInfo> const & tris,
                       float flipThreshold)
        {
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted)
                {
                    continue;
                }
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];

                // Skip triangles shared by both — they will be deleted
                if (ti0 == id1 || ti1 == id1 || ti2 == id1)
                {
                    continue;
                }

                // Substitute id0 → newPos
                Eigen::Vector3f p[3] = {positions[ti0], positions[ti1], positions[ti2]};
                if (ti0 == id0) p[0] = newPos;
                else if (ti1 == id0) p[1] = newPos;
                else if (ti2 == id0) p[2] = newPos;

                Eigen::Vector3f const newN = calcNormal(p[0], p[1], p[2]);
                if (newN.squaredNorm() < 1e-10F)
                {
                    return true; // degenerate
                }
                if (newN.dot(tris[t].normal) < flipThreshold)
                {
                    return true;
                }
            }
            return false;
        }

        /// Count triangles sharing edge (id0, id1)
        std::uint32_t sharedTriCount(std::uint32_t id0,
                                     std::uint32_t id1,
                                     std::vector<std::uint32_t> const & indices,
                                     std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                     std::vector<TriInfo> const & tris)
        {
            std::uint32_t count = 0U;
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                if (ti0 == id1 || ti1 == id1 || ti2 == id1)
                {
                    ++count;
                }
            }
            return count;
        }

        /// Check vertex link condition: the 1-ring neighborhoods of id0 and id1
        /// must share exactly 2 vertices (the two "wing" vertices of the edge's
        /// adjacent triangles). Violating this condition creates non-manifold edges.
        bool linkConditionSatisfied(std::uint32_t id0,
                                    std::uint32_t id1,
                                    std::vector<std::uint32_t> const & indices,
                                    std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                    std::vector<TriInfo> const & tris)
        {
            // Collect unique 1-ring vertices of id0 (excluding id0, id1)
            std::vector<std::uint32_t> ring0;
            ring0.reserve(12U);
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0 && v != id1)
                    {
                        ring0.push_back(v);
                    }
                }
            }
            std::sort(ring0.begin(), ring0.end());
            ring0.erase(std::unique(ring0.begin(), ring0.end()), ring0.end());

            // Collect unique 1-ring vertices of id1, count intersection with ring0
            std::vector<std::uint32_t> ring1;
            ring1.reserve(12U);
            for (auto const t : vtx2tri[id1])
            {
                if (tris[t].deleted) continue;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0 && v != id1)
                    {
                        ring1.push_back(v);
                    }
                }
            }
            std::sort(ring1.begin(), ring1.end());
            ring1.erase(std::unique(ring1.begin(), ring1.end()), ring1.end());

            // Sorted set intersection count
            std::uint32_t commonCount = 0U;
            std::size_t i = 0U;
            std::size_t j = 0U;
            while (i < ring0.size() && j < ring1.size())
            {
                if (ring0[i] < ring1[j])
                {
                    ++i;
                }
                else if (ring0[i] > ring1[j])
                {
                    ++j;
                }
                else
                {
                    ++commonCount;
                    ++i;
                    ++j;
                }
            }
            return commonCount == 2U;
        }

        /// Check if collapsing edge (id0, id1) would create duplicate overlapping
        /// triangles. This happens when a non-shared triangle around id1 (after
        /// rewriting id1→id0) would have the same vertex set as an existing
        /// non-shared triangle around id0. The classic case is the "double-diagonal
        /// diamond": T_a=(id0,W1,W2) and T_b=(id1,W1,W2) both exist alongside
        /// shared triangles (id0,id1,W1) and (id0,id1,W2). After collapse, T_b
        /// becomes (id0,W1,W2), overlapping T_a.
        bool wouldCreateDuplicateFaces(std::uint32_t id0,
                                       std::uint32_t id1,
                                       std::vector<std::uint32_t> const & indices,
                                       std::vector<std::vector<std::uint32_t>> const & vtx2tri,
                                       std::vector<TriInfo> const & tris)
        {
            // Collect non-id0 vertex pairs from non-shared triangles around id0
            std::vector<std::pair<std::uint32_t, std::uint32_t>> id0Pairs;
            id0Pairs.reserve(8U);
            for (auto const t : vtx2tri[id0])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                // Skip shared triangles (contain both id0 and id1)
                if (ti0 == id1 || ti1 == id1 || ti2 == id1) continue;

                std::uint32_t a = UINT32_MAX;
                std::uint32_t b = UINT32_MAX;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id0)
                    {
                        if (a == UINT32_MAX) a = v;
                        else b = v;
                    }
                }
                if (a > b) std::swap(a, b);
                id0Pairs.emplace_back(a, b);
            }

            // Check non-shared triangles around id1 for matching pairs
            for (auto const t : vtx2tri[id1])
            {
                if (tris[t].deleted) continue;
                auto const ti0 = indices[t * 3U + 0U];
                auto const ti1 = indices[t * 3U + 1U];
                auto const ti2 = indices[t * 3U + 2U];
                // Skip shared triangles
                if (ti0 == id0 || ti1 == id0 || ti2 == id0) continue;

                std::uint32_t a = UINT32_MAX;
                std::uint32_t b = UINT32_MAX;
                for (std::uint8_t e = 0U; e < 3U; ++e)
                {
                    auto const v = indices[t * 3U + e];
                    if (v != id1)
                    {
                        if (a == UINT32_MAX) a = v;
                        else b = v;
                    }
                }
                if (a > b) std::swap(a, b);

                for (auto const & p : id0Pairs)
                {
                    if (p.first == a && p.second == b)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// Compact the mesh: remove deleted triangles and unreferenced vertices
        void compactMeshFast(std::vector<Eigen::Vector3f> & positions,
                             std::vector<std::uint32_t> & indices,
                             std::vector<TriInfo> const & tris)
        {
            std::size_t const triCount = indices.size() / 3U;

            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(indices.size());
            for (std::size_t t = 0U; t < triCount; ++t)
            {
                if (tris[t].deleted) continue;
                auto const i0 = indices[t * 3U + 0U];
                auto const i1 = indices[t * 3U + 1U];
                auto const i2 = indices[t * 3U + 2U];
                if (i0 == i1 || i1 == i2 || i2 == i0) continue;
                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
            }

            std::vector<std::uint32_t> remap(positions.size(), UINT32_MAX);
            std::uint32_t newVertCount = 0U;
            for (auto idx : newIndices)
            {
                if (remap[idx] == UINT32_MAX)
                {
                    remap[idx] = newVertCount++;
                }
            }

            std::vector<Eigen::Vector3f> newPositions(newVertCount);
            for (std::size_t i = 0U; i < positions.size(); ++i)
            {
                if (remap[i] != UINT32_MAX)
                {
                    newPositions[remap[i]] = positions[i];
                }
            }

            for (auto & idx : newIndices)
            {
                idx = remap[idx];
            }

            positions = std::move(newPositions);
            indices = std::move(newIndices);
        }
    } // anonymous namespace

    std::size_t fastQemSimplify(
        std::vector<Eigen::Vector3f> & positions,
        std::vector<std::uint32_t> & indices,
        FastQemConfig const & config,
        std::function<void()> throwOnCancel,
        std::function<void(int)> progressFn)
    {
        if (positions.empty() || indices.size() < 3U)
        {
            return 0U;
        }

        std::size_t const initialTriCount = indices.size() / 3U;

        // Determine target triangle count
        std::size_t targetTriCount = 0U;
        switch (config.terminationMode)
        {
        case SimplificationTerminationMode::TargetTriangleCount:
            targetTriCount = config.targetTriangleCount;
            break;
        case SimplificationTerminationMode::TargetReductionPercent:
            targetTriCount = static_cast<std::size_t>(
                static_cast<float>(initialTriCount) * (1.0F - config.targetReductionPercent / 100.0F));
            break;
        case SimplificationTerminationMode::ErrorBounded:
            targetTriCount = 0U;
            break;
        }
        if (targetTriCount >= initialTriCount)
        {
            return 0U;
        }

        std::size_t const collapsesNeeded = initialTriCount - targetTriCount;

        // Weld duplicate vertices: merge vertices at the same position into a
        // single canonical index.  This fixes cracks from dual contouring where
        // different octree cells emit separate vertices at the same location.
        {
            struct Float3Hash
            {
                std::size_t operator()(Eigen::Vector3f const & v) const
                {
                    std::uint32_t hx, hy, hz;
                    std::memcpy(&hx, &v.x(), sizeof(float));
                    std::memcpy(&hy, &v.y(), sizeof(float));
                    std::memcpy(&hz, &v.z(), sizeof(float));
                    // splitmix64-style mixing
                    auto mix = [](std::size_t x)
                    {
                        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
                        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
                        return x ^ (x >> 31);
                    };
                    return mix(hx) ^ (mix(hy) * 2654435761ULL) ^ (mix(hz) * 40503ULL);
                }
            };
            struct Float3Eq
            {
                bool operator()(Eigen::Vector3f const & a, Eigen::Vector3f const & b) const
                {
                    return std::memcmp(&a, &b, sizeof(float) * 3) == 0;
                }
            };

            std::unordered_map<Eigen::Vector3f, std::uint32_t, Float3Hash, Float3Eq> posToIdx;
            posToIdx.reserve(positions.size());
            std::vector<std::uint32_t> remap(positions.size());
            std::size_t mergedCount = 0U;

            for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(positions.size()); ++i)
            {
                auto const [it, inserted] = posToIdx.emplace(positions[i], i);
                remap[i] = it->second;
                if (!inserted && it->second != i) ++mergedCount;
            }

            if (mergedCount > 0U)
            {
                for (auto & idx : indices)
                {
                    idx = remap[idx];
                }
                // Degenerate triangles (with two or more identical indices after
                // welding) are handled downstream: the quadric init marks zero-area
                // triangles as deleted, and compactMeshFast skips them.
            }
        }

        // Build vertex-to-triangle adjacency (vector of vectors for dynamic updates)
        std::vector<std::vector<std::uint32_t>> vtx2tri(positions.size());
        {
            std::size_t const triCount = indices.size() / 3U;
            for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(triCount); ++t)
            {
                vtx2tri[indices[t * 3U + 0U]].push_back(t);
                vtx2tri[indices[t * 3U + 1U]].push_back(t);
                vtx2tri[indices[t * 3U + 2U]].push_back(t);
            }
        }

        // Initialize per-vertex quadrics
        std::vector<SymMat> quadrics(positions.size());
        std::vector<TriInfo> tris(initialTriCount);

        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(initialTriCount); ++t)
        {
            auto const i0 = indices[t * 3U + 0U];
            auto const i1 = indices[t * 3U + 1U];
            auto const i2 = indices[t * 3U + 2U];
            Eigen::Vector3f const n = calcNormal(positions[i0], positions[i1], positions[i2]);
            tris[t].normal = n;
            if (n.squaredNorm() < 1e-10F)
            {
                tris[t].deleted = true;
                continue;
            }
            double const a = n.x(), b = n.y(), c = n.z();
            double const d = -static_cast<double>(n.dot(positions[i0]));
            SymMat const q = SymMat::fromPlane(a, b, c, d);
            quadrics[i0] += q;
            quadrics[i1] += q;
            quadrics[i2] += q;
        }

        // Per-triangle heap index tracking (indexed by triangle index)
        std::vector<std::size_t> triHeapIdx(initialTriCount, std::numeric_limits<std::size_t>::max());

        auto indexSetter = [&triHeapIdx](TriError & te, std::size_t idx)
        {
            te.heapIdx = idx;
            triHeapIdx[te.triIdx] = idx;
        };
        auto lessPred = [](TriError const & a, TriError const & b) { return a.value < b.value; };

        MutablePriorityQueue<TriError, decltype(indexSetter), decltype(lessPred)> pq(indexSetter, lessPred);
        pq.reserve(initialTriCount);

        for (std::uint32_t t = 0U; t < static_cast<std::uint32_t>(initialTriCount); ++t)
        {
            if (tris[t].deleted) continue;
            TriError te;
            te.triIdx = t;
            te.value = evalTriMinEdge(t, indices, positions, quadrics, tris);
            pq.push(te);
        }

        // Greedy collapse loop
        std::size_t totalCollapsed = 0U;
        std::size_t currentTriCount = initialTriCount;
        int lastProgress = -1;

        // Starvation detection: after a successful collapse, re-evaluation may push
        // entries back into the PQ.  If we pop all entries without any successful
        // collapse, the remaining edges are topologically non-collapsible and we
        // should stop.
        std::size_t popsSinceLastCollapse = 0U;
        std::size_t pqSizeAfterLastCollapse = pq.size();

        while (!pq.empty() && currentTriCount > targetTriCount)
        {
            if (throwOnCancel && (totalCollapsed % config.cancelCheckPeriod == 0U))
            {
                throwOnCancel();
            }
            if (progressFn && collapsesNeeded > 0U)
            {
                int const progress = std::min(
                    static_cast<int>((totalCollapsed * 100U) / collapsesNeeded), 100);
                if (progress != lastProgress)
                {
                    progressFn(progress);
                    lastProgress = progress;
                }
            }

            auto const best = pq.top();
            pq.pop();

            // If we have popped more entries than existed after the last
            // successful collapse, all current candidates have been checked
            // and none could be collapsed.  Break to avoid spinning.
            if (++popsSinceLastCollapse > pqSizeAfterLastCollapse)
            {
                break;
            }

            auto const triIdx = best.triIdx;
            if (tris[triIdx].deleted) continue;

            if (best.value > config.maxError) break;

            // Try all three edges of the triangle, starting with the minimum-error
            // edge, then falling back to the other two.  This drastically reduces
            // the number of PQ pops needed because wouldFlip rejects many edges
            // that are optimal by error but geometrically problematic; alternative
            // edges in the same triangle often succeed.
            bool collapsed = false;

            // Sort edges by error for this triangle
            auto const ti0 = indices[triIdx * 3U + 0U];
            auto const ti1 = indices[triIdx * 3U + 1U];
            auto const ti2 = indices[triIdx * 3U + 2U];
            Eigen::Vector3f dummy;
            float const err0 = edgeError(quadrics[ti0], quadrics[ti1],
                                         positions[ti0], positions[ti1], dummy);
            float const err1 = edgeError(quadrics[ti1], quadrics[ti2],
                                         positions[ti1], positions[ti2], dummy);
            float const err2 = edgeError(quadrics[ti2], quadrics[ti0],
                                         positions[ti2], positions[ti0], dummy);

            std::array<std::uint8_t, 3> edgeOrder = {0U, 1U, 2U};
            std::array<float, 3> const edgeErr = {err0, err1, err2};
            std::sort(edgeOrder.begin(), edgeOrder.end(),
                      [&edgeErr](std::uint8_t a, std::uint8_t b)
                      { return edgeErr[a] < edgeErr[b]; });

            for (auto const edgeIdx : edgeOrder)
            {
                if (edgeErr[edgeIdx] > config.maxError) break;

                auto [id0, id1] = triEdgeVerts(triIdx, edgeIdx, indices);

                // Topology guard: only collapse manifold interior edges with valid vertex link
                if (sharedTriCount(id0, id1, indices, vtx2tri, tris) != 2U) continue;
                if (!linkConditionSatisfied(id0, id1, indices, vtx2tri, tris)) continue;
                if (wouldCreateDuplicateFaces(id0, id1, indices, vtx2tri, tris)) continue;

                // Valence guard: the non-surviving vertex (id1) must have at least 3
                // non-deleted triangles. With sharedTriCount == 2, this guarantees at least
                // 1 non-shared triangle exists to fill the gap left by the deleted shared
                // triangles. Without this, collapsing a "tip" vertex whose entire fan
                // consists of only the 2 shared triangles would create boundary edges (a hole).
                // Additionally, the post-collapse valence of the surviving vertex (id0)
                // must be at least 3 to form a valid closed fan.
                {
                    std::uint32_t id0LiveCount = 0U;
                    std::uint32_t id1LiveCount = 0U;
                    for (auto const t : vtx2tri[id0])
                    {
                        if (!tris[t].deleted) ++id0LiveCount;
                    }
                    for (auto const t : vtx2tri[id1])
                    {
                        if (!tris[t].deleted) ++id1LiveCount;
                    }
                    if (id1LiveCount <= 2U) continue;
                    if (id0LiveCount + id1LiveCount < 7U) continue;
                }

                // Wing vertex connectivity guard: for each wing vertex (the third vertex
                // of each shared triangle), verify that the post-collapse edge count at
                // (id0, wing) will be >= 2. The count is: (triangles of id0 with wing - 1
                // for the deleted shared triangle) + (non-shared triangles of id1 with wing).
                // If id0's side has lost triangles from prior collapses, even having a
                // non-shared triangle from id1 may not suffice.
                {
                    bool wingOk = true;
                    for (auto const st : vtx2tri[id1])
                    {
                        if (tris[st].deleted) continue;
                        auto const sv0 = indices[st * 3U + 0U];
                        auto const sv1 = indices[st * 3U + 1U];
                        auto const sv2 = indices[st * 3U + 2U];
                        if (!(sv0 == id0 || sv1 == id0 || sv2 == id0)) continue;
                        // Shared triangle: find the wing vertex
                        std::uint32_t w = UINT32_MAX;
                        for (auto const v : {sv0, sv1, sv2})
                        {
                            if (v != id0 && v != id1) { w = v; break; }
                        }
                        if (w == UINT32_MAX) continue;
                        // Count triangles of id0 containing the wing vertex
                        std::uint32_t id0wCount = 0U;
                        for (auto const t : vtx2tri[id0])
                        {
                            if (tris[t].deleted) continue;
                            auto const x0 = indices[t * 3U + 0U];
                            auto const x1 = indices[t * 3U + 1U];
                            auto const x2 = indices[t * 3U + 2U];
                            if (x0 == w || x1 == w || x2 == w) ++id0wCount;
                        }
                        // Count non-shared triangles of id1 containing the wing vertex
                        std::uint32_t id1wCount = 0U;
                        for (auto const t : vtx2tri[id1])
                        {
                            if (tris[t].deleted) continue;
                            auto const x0 = indices[t * 3U + 0U];
                            auto const x1 = indices[t * 3U + 1U];
                            auto const x2 = indices[t * 3U + 2U];
                            if (x0 == id0 || x1 == id0 || x2 == id0) continue;
                            if (x0 == w || x1 == w || x2 == w) ++id1wCount;
                        }
                        // Post-collapse count for edge (id0, w)
                        if (id0wCount + id1wCount < 3U)
                        {
                            wingOk = false;
                            break;
                        }
                    }
                    if (!wingOk) continue;
                }

                // Compute optimal position
                Eigen::Vector3f newPos;
                edgeError(quadrics[id0], quadrics[id1], positions[id0], positions[id1], newPos);

                // Flip detection
                if (wouldFlip(id0, id1, newPos, positions, indices, vtx2tri, tris, config.flipThreshold))
                {
                    continue;
                }
                if (wouldFlip(id1, id0, newPos, positions, indices, vtx2tri, tris, config.flipThreshold))
                {
                    continue;
                }

                // === Perform collapse: merge id1 into id0 ===
                collapsed = true;
                positions[id0] = newPos;
                quadrics[id0] += quadrics[id1];

                // Mark shared triangles as deleted
                for (auto const t : vtx2tri[id1])
                {
                    if (tris[t].deleted) continue;
                    auto const tti0 = indices[t * 3U + 0U];
                    auto const tti1 = indices[t * 3U + 1U];
                    auto const tti2 = indices[t * 3U + 2U];
                    if (tti0 == id0 || tti1 == id0 || tti2 == id0)
                    {
                        tris[t].deleted = true;
                        --currentTriCount;
                        // Remove from pq (verify the entry still belongs to this triangle)
                        if (triHeapIdx[t] < pq.size() &&
                            pq[triHeapIdx[t]].triIdx == static_cast<std::uint32_t>(t))
                        {
                            pq.remove(triHeapIdx[t]);
                        }
                    }
                }

                // Rewrite id1 → id0 in remaining triangles around id1
                for (auto const t : vtx2tri[id1])
                {
                    if (tris[t].deleted) continue;
                    for (std::uint8_t e2 = 0U; e2 < 3U; ++e2)
                    {
                        if (indices[t * 3U + e2] == id1)
                        {
                            indices[t * 3U + e2] = id0;
                        }
                    }
                    // Add triangle to id0's adjacency
                    vtx2tri[id0].push_back(t);
                }
                vtx2tri[id1].clear();

                // Prune deleted entries from vtx2tri[id0]
                {
                    auto & adj = vtx2tri[id0];
                    adj.erase(std::remove_if(adj.begin(),
                                             adj.end(),
                                             [&tris](std::uint32_t t)
                                             { return tris[t].deleted; }),
                              adj.end());
                }

                // Update normals for triangles around id0
                for (auto const t : vtx2tri[id0])
                {
                    if (tris[t].deleted) continue;
                    auto const ni0 = indices[t * 3U + 0U];
                    auto const ni1 = indices[t * 3U + 1U];
                    auto const ni2 = indices[t * 3U + 2U];
                    tris[t].normal = calcNormal(positions[ni0], positions[ni1], positions[ni2]);
                }

                // Re-evaluate affected triangles in the priority queue
                for (auto const t : vtx2tri[id0])
                {
                    if (tris[t].deleted) continue;
                    float const newErr = evalTriMinEdge(t, indices, positions, quadrics, tris);
                    if (triHeapIdx[t] < pq.size() &&
                        pq[triHeapIdx[t]].triIdx == static_cast<std::uint32_t>(t))
                    {
                        pq[triHeapIdx[t]].value = newErr;
                        pq.update(triHeapIdx[t]);
                    }
                    else
                    {
                        TriError te;
                        te.triIdx = static_cast<std::uint32_t>(t);
                        te.value = newErr;
                        pq.push(te);
                    }
                }

                break; // Edge collapsed successfully, exit inner edge loop
            } // end for (edgeIdx)

            if (!collapsed) continue;

            ++totalCollapsed;

            // Reset starvation counter and record new PQ size
            // (re-evaluation may have pushed entries back).
            popsSinceLastCollapse = 0U;
            pqSizeAfterLastCollapse = pq.size();
        }

        if (progressFn)
        {
            progressFn(100);
        }

        compactMeshFast(positions, indices, tris);

        return totalCollapsed;
    }

} // namespace gladius::compute
