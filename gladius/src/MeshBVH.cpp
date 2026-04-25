/// @file MeshBVH.cpp
/// @brief Implementation of BVH builder for triangle meshes
/// @see MeshBVH.h

#include "MeshBVH.h"
#include "Profiling.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <unordered_map>

namespace gladius
{
    // ========================================================================
    // Build Context
    // ========================================================================

    struct MeshBVHBuilder::BuildContext
    {
        std::vector<MeshTriangle> triangles;
        std::vector<BoundingBox> triangleBounds;
        std::vector<float4> centroids;
        std::vector<int> triangleIndices;  // Original triangle indices for reordering
        std::vector<MeshBVHNode> nodes;
        BoundingBox sceneBounds;
        BoundingBox centroidBounds;

        int totalPrimitives = 0;
        int leafPrimitives = 0;
    };

    // ========================================================================
    // Public Methods
    // ========================================================================

    SpatialMeshData MeshBVHBuilder::build(std::span<float4 const> vertices,
                                          std::span<TriangleIndices const> indices,
                                          MeshBVHBuildParams const & params)
    {
        ProfileFunction;

        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset all diagnostic counters before each build so callers see only the
        // statistics for the current invocation.
        m_lastStats = MeshBVHBuildStats{};

        SpatialMeshData result;

        // Handle empty input
        if (vertices.empty() || indices.empty())
        {
            return result;
        }

        // Initialize build context
        BuildContext ctx;
        size_t const triCount = indices.size();
        ctx.triangles.reserve(triCount);
        ctx.triangleBounds.reserve(triCount);
        ctx.centroids.reserve(triCount);
        ctx.triangleIndices.resize(triCount);
        std::iota(ctx.triangleIndices.begin(), ctx.triangleIndices.end(), 0);

        // Store original triangle indices for vertex normal lookup
        result.triangleIndices.reserve(triCount);

        // Build triangle data
        ctx.sceneBounds = BoundingBox{};
        ctx.centroidBounds = BoundingBox{};

        for (size_t i = 0; i < triCount; ++i)
        {
            TriangleIndices const & idx = indices[i];

            MeshTriangle tri;
            tri.v0 = vertices[idx.i0];
            tri.v1 = vertices[idx.i1];
            tri.v2 = vertices[idx.i2];
            
            // Compute and store face normal (Option D - precomputed normals)
            float e0x = tri.v1.x - tri.v0.x;
            float e0y = tri.v1.y - tri.v0.y;
            float e0z = tri.v1.z - tri.v0.z;
            float e1x = tri.v2.x - tri.v0.x;
            float e1y = tri.v2.y - tri.v0.y;
            float e1z = tri.v2.z - tri.v0.z;
            float nx = e0y * e1z - e0z * e1y;
            float ny = e0z * e1x - e0x * e1z;
            float nz = e0x * e1y - e0y * e1x;
            float lenSq = nx * nx + ny * ny + nz * nz;
            if (lenSq > 1e-10f)
            {
                float invLen = 1.0f / std::sqrt(lenSq);
                nx *= invLen;
                ny *= invLen;
                nz *= invLen;
            }
            tri.faceNormal = float4{nx, ny, nz, 0.0f};
            
            ctx.triangles.push_back(tri);

            // Store vertex indices for this triangle
            result.triangleIndices.push_back(idx);

            // Compute bounds
            BoundingBox bounds = computeTriangleBounds(tri);
            ctx.triangleBounds.push_back(bounds);

            // Compute centroid
            float4 centroid{
                (tri.v0.x + tri.v1.x + tri.v2.x) / 3.0f,
                (tri.v0.y + tri.v1.y + tri.v2.y) / 3.0f,
                (tri.v0.z + tri.v1.z + tri.v2.z) / 3.0f,
                0.0f
            };
            ctx.centroids.push_back(centroid);

            // Expand scene bounds
            ctx.sceneBounds.min.x = std::min(ctx.sceneBounds.min.x, bounds.min.x);
            ctx.sceneBounds.min.y = std::min(ctx.sceneBounds.min.y, bounds.min.y);
            ctx.sceneBounds.min.z = std::min(ctx.sceneBounds.min.z, bounds.min.z);
            ctx.sceneBounds.max.x = std::max(ctx.sceneBounds.max.x, bounds.max.x);
            ctx.sceneBounds.max.y = std::max(ctx.sceneBounds.max.y, bounds.max.y);
            ctx.sceneBounds.max.z = std::max(ctx.sceneBounds.max.z, bounds.max.z);

            // Expand centroid bounds
            ctx.centroidBounds.min.x = std::min(ctx.centroidBounds.min.x, centroid.x);
            ctx.centroidBounds.min.y = std::min(ctx.centroidBounds.min.y, centroid.y);
            ctx.centroidBounds.min.z = std::min(ctx.centroidBounds.min.z, centroid.z);
            ctx.centroidBounds.max.x = std::max(ctx.centroidBounds.max.x, centroid.x);
            ctx.centroidBounds.max.y = std::max(ctx.centroidBounds.max.y, centroid.y);
            ctx.centroidBounds.max.z = std::max(ctx.centroidBounds.max.z, centroid.z);
        }

        // Reserve space for nodes (upper bound: 2n-1 for n primitives)
        ctx.nodes.reserve(2 * triCount);

        // Build BVH recursively (pass scene + centroid bounds down to skip
        // recomputation on the root node).
        buildRecursive(ctx,
                       0,
                       static_cast<int>(triCount),
                       0,
                       params,
                       ctx.sceneBounds,
                       ctx.centroidBounds);

        // Reorder triangles according to BVH leaf order
        result.triangles.resize(triCount);
        std::vector<TriangleIndices> newTriangleIndices(triCount);
        std::vector<int> bvhToOriginalTriangle(triCount);
        for (size_t i = 0; i < triCount; ++i)
        {
            int origIdx = ctx.triangleIndices[i];
            result.triangles[i] = ctx.triangles[origIdx];
            newTriangleIndices[i] = result.triangleIndices[origIdx];
            bvhToOriginalTriangle[i] = origIdx;
        }
        result.triangleIndices = std::move(newTriangleIndices);

        // Copy nodes
        result.nodes = std::move(ctx.nodes);
        result.boundingBox = ctx.sceneBounds;
        result.originalTriangleCount = triCount;

        // Compute angle-weighted vertex normals
        computeAngleWeightedNormals(vertices, indices, result);

        // Compute per-edge adjacent face normals (in BVH order) for robust sign
        // determination on edge features in mesh_sdf.cl::computePseudoNormalFast.
        computeEdgeNeighborNormals(vertices, indices, bvhToOriginalTriangle, result);

        // Compute statistics
        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastStats.buildTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        m_lastStats.totalNodes = static_cast<int>(result.nodes.size());
        m_lastStats.leafNodes = 0;
        m_lastStats.maxDepth = 0;

        int totalLeafPrims = 0;
        for (auto const & node : result.nodes)
        {
            if (node.isLeaf())
            {
                ++m_lastStats.leafNodes;
                totalLeafPrims += node.primCount;
            }
        }

        m_lastStats.avgPrimitivesPerLeaf = m_lastStats.leafNodes > 0 
            ? static_cast<float>(totalLeafPrims) / static_cast<float>(m_lastStats.leafNodes)
            : 0.0f;

        return result;
    }

    // ========================================================================
    // Private Methods
    // ========================================================================

    void MeshBVHBuilder::computeAngleWeightedNormals(std::span<float4 const> vertices,
                                                     std::span<TriangleIndices const> indices,
                                                     SpatialMeshData & data)
    {
        ProfileFunction;

        size_t const vertexCount = vertices.size();
        data.vertexNormals.resize(vertexCount);

        // Initialize all normals to zero
        for (auto & vn : data.vertexNormals)
        {
            vn.normal = {0.0f, 0.0f, 0.0f, 0.0f};
        }

        // Accumulate angle-weighted face normals for each vertex
        for (size_t i = 0; i < indices.size(); ++i)
        {
            TriangleIndices const & idx = indices[i];
            float4 const & p0 = vertices[idx.i0];
            float4 const & p1 = vertices[idx.i1];
            float4 const & p2 = vertices[idx.i2];

            // Edge vectors
            float e0x = p1.x - p0.x, e0y = p1.y - p0.y, e0z = p1.z - p0.z;
            float e1x = p2.x - p0.x, e1y = p2.y - p0.y, e1z = p2.z - p0.z;
            float e2x = p2.x - p1.x, e2y = p2.y - p1.y, e2z = p2.z - p1.z;

            // Face normal (not normalized, magnitude = 2 * area)
            float fnx = e0y * e1z - e0z * e1y;
            float fny = e0z * e1x - e0x * e1z;
            float fnz = e0x * e1y - e0y * e1x;

            // Compute edge lengths
            float len_e0 = std::sqrt(e0x * e0x + e0y * e0y + e0z * e0z);
            float len_e1 = std::sqrt(e1x * e1x + e1y * e1y + e1z * e1z);
            float len_e2 = std::sqrt(e2x * e2x + e2y * e2y + e2z * e2z);

            // Avoid division by zero for degenerate triangles
            if (len_e0 < 1e-10f || len_e1 < 1e-10f || len_e2 < 1e-10f)
            {
                ++m_lastStats.degenerateTriangleCount;
                continue;
            }

            // Normalise the face normal so the per-vertex contribution is weighted purely by
            // the corner angle (Bærentzen & Aanæs 2005). Without this, contributions are
            // additionally scaled by 2 * area, which biases the vertex pseudo-normal toward
            // large adjacent faces and produces sign artifacts near sharp creases.
            float const faceNormalLenSq = fnx * fnx + fny * fny + fnz * fnz;
            if (faceNormalLenSq < 1e-20f)
            {
                ++m_lastStats.degenerateTriangleCount;
                continue;
            }
            float const invFaceNormalLen = 1.0f / std::sqrt(faceNormalLenSq);
            fnx *= invFaceNormalLen;
            fny *= invFaceNormalLen;
            fnz *= invFaceNormalLen;

            // Angle at vertex 0: between e0 and e1
            float dot01 = e0x * e1x + e0y * e1y + e0z * e1z;
            float cosAngle0 = dot01 / (len_e0 * len_e1);
            float angle0 = std::acos(std::clamp(cosAngle0, -1.0f, 1.0f));

            // Angle at vertex 1: between -e0 and e2
            float neg_e0_dot_e2 = (-e0x) * e2x + (-e0y) * e2y + (-e0z) * e2z;
            float cosAngle1 = neg_e0_dot_e2 / (len_e0 * len_e2);
            float angle1 = std::acos(std::clamp(cosAngle1, -1.0f, 1.0f));

            // Angle at vertex 2: remaining angle (angles sum to pi)
            float angle2 = std::numbers::pi_v<float> - angle0 - angle1;

            // Add weighted face normal to each vertex
            data.vertexNormals[idx.i0].normal.x += angle0 * fnx;
            data.vertexNormals[idx.i0].normal.y += angle0 * fny;
            data.vertexNormals[idx.i0].normal.z += angle0 * fnz;

            data.vertexNormals[idx.i1].normal.x += angle1 * fnx;
            data.vertexNormals[idx.i1].normal.y += angle1 * fny;
            data.vertexNormals[idx.i1].normal.z += angle1 * fnz;

            data.vertexNormals[idx.i2].normal.x += angle2 * fnx;
            data.vertexNormals[idx.i2].normal.y += angle2 * fny;
            data.vertexNormals[idx.i2].normal.z += angle2 * fnz;
        }

        // Normalize all vertex normals
        for (size_t i = 0; i < vertexCount; ++i)
        {
            auto & n = data.vertexNormals[i].normal;
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 1e-10f)
            {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            }
            // Store vertex index in w component
            n.w = static_cast<float>(i);
        }
    }

    void MeshBVHBuilder::computeEdgeNeighborNormals(std::span<float4 const> vertices,
                                                    std::span<TriangleIndices const> originalIndices,
                                                    std::vector<int> const & bvhToOriginalTriangle,
                                                    SpatialMeshData & data)
    {
        ProfileFunction;

        size_t const triCount = originalIndices.size();
        data.edgeNeighborNormals.assign(triCount * 3u, MeshEdgeNeighborNormal{});

        if (triCount == 0u)
        {
            return;
        }

        // Edge index convention (must match sqTriangleWithClosestPoint in mesh_sdf.cl):
        //   edge 0 = v0–v1, edge 1 = v1–v2, edge 2 = v0–v2.
        auto const edgeVertices = [](TriangleIndices const & idx, int edge) -> std::pair<uint32_t, uint32_t>
        {
            switch (edge)
            {
            case 0: return {idx.i0, idx.i1};
            case 1: return {idx.i1, idx.i2};
            default: return {idx.i0, idx.i2};
            }
        };

        // Precompute unit face normals in original triangle order
        std::vector<float4> faceNormals(triCount, float4{0.f, 0.f, 0.f, 0.f});
        for (size_t t = 0; t < triCount; ++t)
        {
            TriangleIndices const & idx = originalIndices[t];
            float4 const & p0 = vertices[idx.i0];
            float4 const & p1 = vertices[idx.i1];
            float4 const & p2 = vertices[idx.i2];

            float const e0x = p1.x - p0.x, e0y = p1.y - p0.y, e0z = p1.z - p0.z;
            float const e1x = p2.x - p0.x, e1y = p2.y - p0.y, e1z = p2.z - p0.z;
            float fnx = e0y * e1z - e0z * e1y;
            float fny = e0z * e1x - e0x * e1z;
            float fnz = e0x * e1y - e0y * e1x;
            float const lenSq = fnx * fnx + fny * fny + fnz * fnz;
            if (lenSq < 1e-20f)
            {
                continue;
            }
            float const invLen = 1.0f / std::sqrt(lenSq);
            faceNormals[t] = float4{fnx * invLen, fny * invLen, fnz * invLen, 1.0f};
        }

        // Hash undirected edges to (triIdx, edgeIdx). On second hit we know both sides.
        struct EdgeKey
        {
            uint64_t key;
            bool operator==(EdgeKey const & o) const { return key == o.key; }
        };
        struct EdgeKeyHash
        {
            size_t operator()(EdgeKey const & k) const noexcept { return std::hash<uint64_t>{}(k.key); }
        };

        auto const makeKey = [](uint32_t a, uint32_t b) -> EdgeKey
        {
            uint32_t lo = std::min(a, b);
            uint32_t hi = std::max(a, b);
            return EdgeKey{(static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo)};
        };

        struct EdgeRef
        {
            int triIdx;
            int edgeIdx;
        };

        // Collect ALL incident (triangle, edge) refs per undirected edge so that we can
        // resolve boundary, manifold and non-manifold edges in a deterministic post-pass.
        std::unordered_map<EdgeKey, std::vector<EdgeRef>, EdgeKeyHash> edgeMap;
        edgeMap.reserve(triCount * 3u);

        // Result in original-triangle order; reorder at the end.
        std::vector<MeshEdgeNeighborNormal> origOrder(triCount * 3u);

        for (size_t t = 0; t < triCount; ++t)
        {
            TriangleIndices const & idx = originalIndices[t];
            for (int e = 0; e < 3; ++e)
            {
                auto [va, vb] = edgeVertices(idx, e);
                EdgeKey const key = makeKey(va, vb);
                edgeMap[key].push_back(EdgeRef{static_cast<int>(t), e});
            }
        }

        auto const writeNeighbor = [&](EdgeRef const & lhs, float4 const & neighborNormal)
        {
            origOrder[static_cast<size_t>(lhs.triIdx) * 3u + static_cast<size_t>(lhs.edgeIdx)].normal =
                neighborNormal;
        };

        for (auto const & [key, refs] : edgeMap)
        {
            (void) key;
            if (refs.size() == 1u)
            {
                // Boundary edge: leave neighbour normal at zero (already initialised).
                ++m_lastStats.boundaryEdgeCount;
            }
            else if (refs.size() == 2u)
            {
                writeNeighbor(refs[0], faceNormals[refs[1].triIdx]);
                writeNeighbor(refs[1], faceNormals[refs[0].triIdx]);
            }
            else
            {
                // Non-manifold edge (>= 3 incident faces): pick the pair with the
                // smallest dihedral angle (largest dot product between unit face normals,
                // i.e. closest to coplanar) as the most plausible surface continuation.
                // The remaining incident faces stay at zero — they are typically
                // intersecting geometry where no consistent neighbour exists.
                ++m_lastStats.nonManifoldEdgeCount;

                size_t bestI = 0u;
                size_t bestJ = 1u;
                float bestDot = -2.f;
                for (size_t i = 0u; i < refs.size(); ++i)
                {
                    float4 const & ni = faceNormals[refs[i].triIdx];
                    if (ni.w == 0.f)
                    {
                        continue; // skip degenerate face
                    }
                    for (size_t j = i + 1u; j < refs.size(); ++j)
                    {
                        float4 const & nj = faceNormals[refs[j].triIdx];
                        if (nj.w == 0.f)
                        {
                            continue;
                        }
                        // For an undirected edge shared by two outward-facing triangles,
                        // the normals on the two sides can be either nearly parallel
                        // (smooth surface) or anti-parallel (sharp fold). Use |dot| so
                        // both count as "small dihedral".
                        float const d = std::abs(ni.x * nj.x + ni.y * nj.y + ni.z * nj.z);
                        if (d > bestDot)
                        {
                            bestDot = d;
                            bestI = i;
                            bestJ = j;
                        }
                    }
                }
                if (bestDot > -2.f)
                {
                    writeNeighbor(refs[bestI], faceNormals[refs[bestJ].triIdx]);
                    writeNeighbor(refs[bestJ], faceNormals[refs[bestI].triIdx]);
                }
            }
        }

        // Reorder into BVH triangle order
        for (size_t i = 0; i < triCount; ++i)
        {
            int const orig = bvhToOriginalTriangle[i];
            for (int e = 0; e < 3; ++e)
            {
                data.edgeNeighborNormals[i * 3u + static_cast<size_t>(e)] =
                    origOrder[static_cast<size_t>(orig) * 3u + static_cast<size_t>(e)];
            }
        }
    }

    BoundingBox MeshBVHBuilder::computeTriangleBounds(MeshTriangle const & tri)
    {
        BoundingBox bounds;
        bounds.min.x = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
        bounds.min.y = std::min({tri.v0.y, tri.v1.y, tri.v2.y});
        bounds.min.z = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
        bounds.min.w = 0.0f;

        bounds.max.x = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
        bounds.max.y = std::max({tri.v0.y, tri.v1.y, tri.v2.y});
        bounds.max.z = std::max({tri.v0.z, tri.v1.z, tri.v2.z});
        bounds.max.w = 0.0f;

        return bounds;
    }

    int MeshBVHBuilder::buildRecursive(BuildContext & ctx,
                                       int start,
                                       int end,
                                       int depth,
                                       MeshBVHBuildParams const & params,
                                       BoundingBox const & nodeBounds,
                                       BoundingBox const & centroidBounds)
    {
        int const primCount = end - start;

        // Allocate node and write its bbox up-front. Note: ctx.nodes was reserved to
        // upper bound (2*triCount) in build(), so this reference remains valid across
        // recursive emplace_back calls.
        int const nodeIndex = static_cast<int>(ctx.nodes.size());
        ctx.nodes.emplace_back();
        ctx.nodes[nodeIndex].bboxMin = nodeBounds.min;
        ctx.nodes[nodeIndex].bboxMax = nodeBounds.max;

        auto const makeLeaf = [&]()
        {
            ctx.nodes[nodeIndex].leftChild = -1;
            ctx.nodes[nodeIndex].rightChild = -1;
            ctx.nodes[nodeIndex].primStart = start;
            ctx.nodes[nodeIndex].primCount = primCount;
        };

        if (primCount <= params.maxPrimitivesPerLeaf || depth >= params.maxDepth)
        {
            makeLeaf();
            return nodeIndex;
        }

        float const parentArea = surfaceArea(nodeBounds);
        if (parentArea < 1e-10f)
        {
            makeLeaf();
            return nodeIndex;
        }

        // ----------------------------------------------------------------
        // Single-pass 3-axis binning. All 3 axes are filled in one traversal
        // of the primitive range, eliminating the previous 3 separate sweeps.
        // Bins are stack-allocated (no per-call vector heap allocation).
        // ----------------------------------------------------------------
        constexpr int kNumBins = 32;
        struct Bin
        {
            int count = 0;
            BoundingBox bounds{};
        };
        Bin bins[3][kNumBins];

        float const axisMin[3] = {centroidBounds.min.x, centroidBounds.min.y, centroidBounds.min.z};
        float const axisMax[3] = {centroidBounds.max.x, centroidBounds.max.y, centroidBounds.max.z};
        float axisExtent[3];
        bool axisActive[3];
        float axisScale[3];
        for (int a = 0; a < 3; ++a)
        {
            axisExtent[a] = axisMax[a] - axisMin[a];
            axisActive[a] = axisExtent[a] > 1e-10f;
            axisScale[a] = axisActive[a] ? static_cast<float>(kNumBins) / axisExtent[a] : 0.0f;
        }

        if (!axisActive[0] && !axisActive[1] && !axisActive[2])
        {
            // All centroids coincide: cannot split meaningfully.
            makeLeaf();
            return nodeIndex;
        }

        for (int i = start; i < end; ++i)
        {
            int const origIdx = ctx.triangleIndices[i];
            float4 const & cen = ctx.centroids[origIdx];
            BoundingBox const & b = ctx.triangleBounds[origIdx];
            float const c[3] = {cen.x, cen.y, cen.z};

            for (int a = 0; a < 3; ++a)
            {
                if (!axisActive[a])
                {
                    continue;
                }
                int binIdx = static_cast<int>((c[a] - axisMin[a]) * axisScale[a]);
                if (binIdx < 0) binIdx = 0;
                if (binIdx >= kNumBins) binIdx = kNumBins - 1;
                Bin & bin = bins[a][binIdx];
                ++bin.count;
                bin.bounds.min.x = std::min(bin.bounds.min.x, b.min.x);
                bin.bounds.min.y = std::min(bin.bounds.min.y, b.min.y);
                bin.bounds.min.z = std::min(bin.bounds.min.z, b.min.z);
                bin.bounds.max.x = std::max(bin.bounds.max.x, b.max.x);
                bin.bounds.max.y = std::max(bin.bounds.max.y, b.max.y);
                bin.bounds.max.z = std::max(bin.bounds.max.z, b.max.z);
            }
        }

        auto const expandBoundsInto = [](BoundingBox & dst, BoundingBox const & src)
        {
            dst.min.x = std::min(dst.min.x, src.min.x);
            dst.min.y = std::min(dst.min.y, src.min.y);
            dst.min.z = std::min(dst.min.z, src.min.z);
            dst.max.x = std::max(dst.max.x, src.max.x);
            dst.max.y = std::max(dst.max.y, src.max.y);
            dst.max.z = std::max(dst.max.z, src.max.z);
        };

        // ----------------------------------------------------------------
        // SAH evaluation via O(numBins) prefix/suffix sweeps per axis instead
        // of the previous O(numBins^2) split scan.
        // ----------------------------------------------------------------
        float bestCost = std::numeric_limits<float>::max();
        int bestAxis = -1;
        int bestSplitBin = -1;
        BoundingBox bestLeftBounds{};
        BoundingBox bestRightBounds{};

        BoundingBox prefixBounds[kNumBins];
        int prefixCount[kNumBins];
        BoundingBox suffixBounds[kNumBins];
        int suffixCount[kNumBins];

        for (int a = 0; a < 3; ++a)
        {
            if (!axisActive[a])
            {
                continue;
            }

            BoundingBox accBounds{};
            int accCount = 0;
            for (int i = 0; i < kNumBins; ++i)
            {
                if (bins[a][i].count > 0)
                {
                    accCount += bins[a][i].count;
                    expandBoundsInto(accBounds, bins[a][i].bounds);
                }
                prefixBounds[i] = accBounds;
                prefixCount[i] = accCount;
            }

            accBounds = BoundingBox{};
            accCount = 0;
            for (int i = kNumBins - 1; i >= 0; --i)
            {
                if (bins[a][i].count > 0)
                {
                    accCount += bins[a][i].count;
                    expandBoundsInto(accBounds, bins[a][i].bounds);
                }
                suffixBounds[i] = accBounds;
                suffixCount[i] = accCount;
            }

            for (int i = 0; i < kNumBins - 1; ++i)
            {
                int const leftCount = prefixCount[i];
                int const rightCount = suffixCount[i + 1];
                if (leftCount == 0 || rightCount == 0)
                {
                    continue;
                }

                float const cost = params.traversalCost +
                    (surfaceArea(prefixBounds[i]) * leftCount +
                     surfaceArea(suffixBounds[i + 1]) * rightCount) *
                    params.intersectionCost / parentArea;

                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = a;
                    bestSplitBin = i;
                    bestLeftBounds = prefixBounds[i];
                    bestRightBounds = suffixBounds[i + 1];
                }
            }
        }

        float const leafCost = primCount * params.intersectionCost;
        if (bestAxis < 0 || bestCost >= leafCost)
        {
            makeLeaf();
            return nodeIndex;
        }

        // ----------------------------------------------------------------
        // Partition by chosen split. std::partition is O(n) and stable enough
        // for our use (BVH order doesn't depend on pre-existing order). The
        // predicate must reproduce the same binning used in the SAH eval to
        // keep prefix/suffix counts and per-side bounds consistent.
        // ----------------------------------------------------------------
        int const splitAxis = bestAxis;
        float const splitMin = axisMin[splitAxis];
        float const splitScale = axisScale[splitAxis];
        int const splitBin = bestSplitBin;

        auto const partIt = std::partition(
            ctx.triangleIndices.begin() + start,
            ctx.triangleIndices.begin() + end,
            [&ctx, splitAxis, splitMin, splitScale, splitBin](int idx)
            {
                float4 const & cen = ctx.centroids[idx];
                float const c = (splitAxis == 0) ? cen.x : (splitAxis == 1) ? cen.y : cen.z;
                int binIdx = static_cast<int>((c - splitMin) * splitScale);
                if (binIdx < 0) binIdx = 0;
                if (binIdx >= kNumBins) binIdx = kNumBins - 1;
                return binIdx <= splitBin;
            });

        int mid = static_cast<int>(partIt - ctx.triangleIndices.begin());
        if (mid == start || mid == end)
        {
            // Degenerate partition (all primitives ended up on one side, e.g. due
            // to floating-point edge cases at bin boundaries). Fall back to a median
            // split via nth_element so we still make progress.
            mid = start + primCount / 2;
            std::nth_element(
                ctx.triangleIndices.begin() + start,
                ctx.triangleIndices.begin() + mid,
                ctx.triangleIndices.begin() + end,
                [&ctx, splitAxis](int a, int b)
                {
                    float4 const & ca = ctx.centroids[a];
                    float4 const & cb = ctx.centroids[b];
                    float const va = (splitAxis == 0) ? ca.x : (splitAxis == 1) ? ca.y : ca.z;
                    float const vb = (splitAxis == 0) ? cb.x : (splitAxis == 1) ? cb.y : cb.z;
                    return va < vb;
                });
            // Recompute child bounds from scratch since the SAH-derived ones no
            // longer match this split.
            bestLeftBounds = BoundingBox{};
            bestRightBounds = BoundingBox{};
            for (int i = start; i < mid; ++i)
            {
                expandBoundsInto(bestLeftBounds, ctx.triangleBounds[ctx.triangleIndices[i]]);
            }
            for (int i = mid; i < end; ++i)
            {
                expandBoundsInto(bestRightBounds, ctx.triangleBounds[ctx.triangleIndices[i]]);
            }
        }

        // Compute child centroid bounds in one pass each. We already have the
        // child node-bounds from the SAH sweep (or the fallback above).
        BoundingBox leftCentroidBounds{};
        BoundingBox rightCentroidBounds{};
        for (int i = start; i < mid; ++i)
        {
            float4 const & c = ctx.centroids[ctx.triangleIndices[i]];
            leftCentroidBounds.min.x = std::min(leftCentroidBounds.min.x, c.x);
            leftCentroidBounds.min.y = std::min(leftCentroidBounds.min.y, c.y);
            leftCentroidBounds.min.z = std::min(leftCentroidBounds.min.z, c.z);
            leftCentroidBounds.max.x = std::max(leftCentroidBounds.max.x, c.x);
            leftCentroidBounds.max.y = std::max(leftCentroidBounds.max.y, c.y);
            leftCentroidBounds.max.z = std::max(leftCentroidBounds.max.z, c.z);
        }
        for (int i = mid; i < end; ++i)
        {
            float4 const & c = ctx.centroids[ctx.triangleIndices[i]];
            rightCentroidBounds.min.x = std::min(rightCentroidBounds.min.x, c.x);
            rightCentroidBounds.min.y = std::min(rightCentroidBounds.min.y, c.y);
            rightCentroidBounds.min.z = std::min(rightCentroidBounds.min.z, c.z);
            rightCentroidBounds.max.x = std::max(rightCentroidBounds.max.x, c.x);
            rightCentroidBounds.max.y = std::max(rightCentroidBounds.max.y, c.y);
            rightCentroidBounds.max.z = std::max(rightCentroidBounds.max.z, c.z);
        }

        ctx.nodes[nodeIndex].primStart = 0;
        ctx.nodes[nodeIndex].primCount = 0;
        int const leftIdx = buildRecursive(ctx, start, mid, depth + 1, params,
                                           bestLeftBounds, leftCentroidBounds);
        int const rightIdx = buildRecursive(ctx, mid, end, depth + 1, params,
                                            bestRightBounds, rightCentroidBounds);
        ctx.nodes[nodeIndex].leftChild = leftIdx;
        ctx.nodes[nodeIndex].rightChild = rightIdx;

        return nodeIndex;
    }

    float MeshBVHBuilder::surfaceArea(BoundingBox const & box)
    {
        float dx = box.max.x - box.min.x;
        float dy = box.max.y - box.min.y;
        float dz = box.max.z - box.min.z;

        if (dx < 0 || dy < 0 || dz < 0)
        {
            return 0.0f;
        }

        return 2.0f * (dx * dy + dy * dz + dz * dx);
    }

    // ========================================================================
    // Fast-Winding-Number aggregate computation (Barill et al. 2018)
    // ========================================================================

    void computeFwnAggregates(SpatialMeshData & data)
    {
        ProfileFunction;

        data.fwnAggregates.clear();
        if (data.nodes.empty())
        {
            return;
        }
        data.fwnAggregates.resize(data.nodes.size());

        // Recursive bottom-up accumulation. Iterative form using an explicit
        // post-order stack keeps the call depth bounded for huge BVHs.
        struct Frame
        {
            int nodeIndex;
            bool childrenProcessed;
        };
        std::vector<Frame> stack;
        stack.reserve(static_cast<std::size_t>(64));
        stack.push_back({0, false});

        while (!stack.empty())
        {
            Frame & top = stack.back();
            MeshBVHNode const & node = data.nodes[static_cast<std::size_t>(top.nodeIndex)];

            if (node.isLeaf())
            {
                MeshBVHFwnAggregate aggregate{};
                float weightedSumX = 0.f;
                float weightedSumY = 0.f;
                float weightedSumZ = 0.f;
                float centroidX = 0.f;
                float centroidY = 0.f;
                float centroidZ = 0.f;
                float totalArea = 0.f;
                int const start = node.primStart;
                int const end = start + node.primCount;
                for (int i = start; i < end; ++i)
                {
                    MeshTriangle const & tri = data.triangles[static_cast<std::size_t>(i)];
                    float const ex = tri.v1.x - tri.v0.x;
                    float const ey = tri.v1.y - tri.v0.y;
                    float const ez = tri.v1.z - tri.v0.z;
                    float const fx = tri.v2.x - tri.v0.x;
                    float const fy = tri.v2.y - tri.v0.y;
                    float const fz = tri.v2.z - tri.v0.z;
                    // Cross(e, f); 0.5 * |cross| is the area; faceNormal is unit.
                    float const cx = ey * fz - ez * fy;
                    float const cy = ez * fx - ex * fz;
                    float const cz = ex * fy - ey * fx;
                    float const area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
                    // Use 2·area·n  (collapses to raw cross/2 contributions: simpler GPU formula).
                    float const w2a = 2.f * area;
                    weightedSumX += w2a * tri.faceNormal.x;
                    weightedSumY += w2a * tri.faceNormal.y;
                    weightedSumZ += w2a * tri.faceNormal.z;
                    float const cxC = (tri.v0.x + tri.v1.x + tri.v2.x) * (1.f / 3.f);
                    float const cyC = (tri.v0.y + tri.v1.y + tri.v2.y) * (1.f / 3.f);
                    float const czC = (tri.v0.z + tri.v1.z + tri.v2.z) * (1.f / 3.f);
                    centroidX += area * cxC;
                    centroidY += area * cyC;
                    centroidZ += area * czC;
                    totalArea += area;
                }

                aggregate.weightedNormalSum = {weightedSumX, weightedSumY, weightedSumZ, 0.f};
                aggregate.areaCentroid = {centroidX, centroidY, centroidZ, totalArea};

                // Bounding radius: distance from area-weighted centroid to the
                // farthest enclosed vertex.
                float radiusSq = 0.f;
                if (totalArea > 0.f)
                {
                    float const ax = centroidX / totalArea;
                    float const ay = centroidY / totalArea;
                    float const az = centroidZ / totalArea;
                    for (int i = start; i < end; ++i)
                    {
                        MeshTriangle const & tri = data.triangles[static_cast<std::size_t>(i)];
                        for (int v = 0; v < 3; ++v)
                        {
                            float4 const & pv = (v == 0) ? tri.v0 : (v == 1 ? tri.v1 : tri.v2);
                            float const dx = pv.x - ax;
                            float const dy = pv.y - ay;
                            float const dz = pv.z - az;
                            float const d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 > radiusSq)
                            {
                                radiusSq = d2;
                            }
                        }
                    }
                }
                aggregate.weightedNormalSum.w = std::sqrt(radiusSq);
                data.fwnAggregates[static_cast<std::size_t>(top.nodeIndex)] = aggregate;
                stack.pop_back();
                continue;
            }

            if (!top.childrenProcessed)
            {
                top.childrenProcessed = true;
                int const left = node.leftChild;
                int const right = node.rightChild;
                if (right >= 0)
                {
                    stack.push_back({right, false});
                }
                if (left >= 0)
                {
                    stack.push_back({left, false});
                }
                continue;
            }

            // Both children done — combine.
            MeshBVHFwnAggregate combined{};
            float radiusSq = 0.f;
            float ax = 0.f, ay = 0.f, az = 0.f;
            for (int childIdx : {node.leftChild, node.rightChild})
            {
                if (childIdx < 0)
                {
                    continue;
                }
                MeshBVHFwnAggregate const & ch =
                    data.fwnAggregates[static_cast<std::size_t>(childIdx)];
                combined.weightedNormalSum.x += ch.weightedNormalSum.x;
                combined.weightedNormalSum.y += ch.weightedNormalSum.y;
                combined.weightedNormalSum.z += ch.weightedNormalSum.z;
                combined.areaCentroid.x += ch.areaCentroid.x;
                combined.areaCentroid.y += ch.areaCentroid.y;
                combined.areaCentroid.z += ch.areaCentroid.z;
                combined.areaCentroid.w += ch.areaCentroid.w;
            }
            if (combined.areaCentroid.w > 0.f)
            {
                ax = combined.areaCentroid.x / combined.areaCentroid.w;
                ay = combined.areaCentroid.y / combined.areaCentroid.w;
                az = combined.areaCentroid.z / combined.areaCentroid.w;
                // Combined radius = max over children of (distance from new centroid
                // to child centroid + child radius). Conservative upper bound that
                // still gives a tight Barnes-Hut threshold.
                for (int childIdx : {node.leftChild, node.rightChild})
                {
                    if (childIdx < 0)
                    {
                        continue;
                    }
                    MeshBVHFwnAggregate const & ch =
                        data.fwnAggregates[static_cast<std::size_t>(childIdx)];
                    if (ch.areaCentroid.w <= 0.f)
                    {
                        continue;
                    }
                    float const cx = ch.areaCentroid.x / ch.areaCentroid.w;
                    float const cy = ch.areaCentroid.y / ch.areaCentroid.w;
                    float const cz = ch.areaCentroid.z / ch.areaCentroid.w;
                    float const dx = cx - ax;
                    float const dy = cy - ay;
                    float const dz = cz - az;
                    float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    float const total = dist + ch.weightedNormalSum.w;
                    if (total * total > radiusSq)
                    {
                        radiusSq = total * total;
                    }
                }
            }
            combined.weightedNormalSum.w = std::sqrt(radiusSq);
            data.fwnAggregates[static_cast<std::size_t>(top.nodeIndex)] = combined;
            stack.pop_back();
        }
    }

}  // namespace gladius
