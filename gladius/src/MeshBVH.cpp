/// @file MeshBVH.cpp
/// @brief Implementation of BVH builder for triangle meshes
/// @see MeshBVH.h

#include "MeshBVH.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

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
        auto startTime = std::chrono::high_resolution_clock::now();

        SpatialMeshData result;

        // Handle empty input
        if (vertices.empty() || indices.empty())
        {
            m_lastStats = MeshBVHBuildStats{};
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

        // Build BVH recursively
        buildRecursive(ctx, 0, static_cast<int>(triCount), 0, params);

        // Reorder triangles according to BVH leaf order
        result.triangles.resize(triCount);
        std::vector<TriangleIndices> newTriangleIndices(triCount);
        for (size_t i = 0; i < triCount; ++i)
        {
            int origIdx = ctx.triangleIndices[i];
            result.triangles[i] = ctx.triangles[origIdx];
            newTriangleIndices[i] = result.triangleIndices[origIdx];
        }
        result.triangleIndices = std::move(newTriangleIndices);

        // Copy nodes
        result.nodes = std::move(ctx.nodes);
        result.boundingBox = ctx.sceneBounds;
        result.originalTriangleCount = triCount;

        // Compute angle-weighted vertex normals
        computeAngleWeightedNormals(vertices, indices, result);

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
                continue;
            }

            // Angle at vertex 0: between e0 and e1
            float dot01 = e0x * e1x + e0y * e1y + e0z * e1z;
            float cosAngle0 = dot01 / (len_e0 * len_e1);
            float angle0 = std::acos(std::clamp(cosAngle0, -1.0f, 1.0f));

            // Angle at vertex 1: between -e0 and e2
            float neg_e0_dot_e2 = (-e0x) * e2x + (-e0y) * e2y + (-e0z) * e2z;
            float cosAngle1 = neg_e0_dot_e2 / (len_e0 * len_e2);
            float angle1 = std::acos(std::clamp(cosAngle1, -1.0f, 1.0f));

            // Angle at vertex 2: remaining angle (angles sum to pi)
            float angle2 = 3.14159265359f - angle0 - angle1;

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
                                       MeshBVHBuildParams const & params)
    {
        int const primCount = end - start;

        // Create node
        int const nodeIndex = static_cast<int>(ctx.nodes.size());
        ctx.nodes.emplace_back();
        MeshBVHNode & node = ctx.nodes[nodeIndex];

        // Compute bounds for this node
        BoundingBox nodeBounds{};
        for (int i = start; i < end; ++i)
        {
            int origIdx = ctx.triangleIndices[i];
            auto const & b = ctx.triangleBounds[origIdx];
            nodeBounds.min.x = std::min(nodeBounds.min.x, b.min.x);
            nodeBounds.min.y = std::min(nodeBounds.min.y, b.min.y);
            nodeBounds.min.z = std::min(nodeBounds.min.z, b.min.z);
            nodeBounds.max.x = std::max(nodeBounds.max.x, b.max.x);
            nodeBounds.max.y = std::max(nodeBounds.max.y, b.max.y);
            nodeBounds.max.z = std::max(nodeBounds.max.z, b.max.z);
        }

        node.bboxMin = nodeBounds.min;
        node.bboxMax = nodeBounds.max;

        // Check if should create leaf
        if (primCount <= params.maxPrimitivesPerLeaf || depth >= params.maxDepth)
        {
            node.leftChild = -1;
            node.rightChild = -1;
            node.primStart = start;
            node.primCount = primCount;
            return nodeIndex;
        }

        // Find best split using SAH
        // Use binned SAH for efficiency
        constexpr int numBins = 32;

        float bestCost = std::numeric_limits<float>::max();
        int bestAxis = 0;
        int bestSplit = start + primCount / 2;

        float const parentArea = surfaceArea(nodeBounds);
        if (parentArea < 1e-10f)
        {
            // Degenerate case: create leaf
            node.leftChild = -1;
            node.rightChild = -1;
            node.primStart = start;
            node.primCount = primCount;
            return nodeIndex;
        }

        // Try each axis
        for (int axis = 0; axis < 3; ++axis)
        {
            // Compute centroid extent on this axis
            float minC = std::numeric_limits<float>::max();
            float maxC = -std::numeric_limits<float>::max();
            for (int i = start; i < end; ++i)
            {
                int origIdx = ctx.triangleIndices[i];
                float c = (axis == 0) ? ctx.centroids[origIdx].x :
                          (axis == 1) ? ctx.centroids[origIdx].y :
                                        ctx.centroids[origIdx].z;
                minC = std::min(minC, c);
                maxC = std::max(maxC, c);
            }

            if (maxC - minC < 1e-10f)
            {
                continue;  // No split possible on this axis
            }

            // Bin primitives
            struct Bin
            {
                int count = 0;
                BoundingBox bounds{};
            };
            std::vector<Bin> bins(numBins);

            float scale = static_cast<float>(numBins) / (maxC - minC);
            for (int i = start; i < end; ++i)
            {
                int origIdx = ctx.triangleIndices[i];
                float c = (axis == 0) ? ctx.centroids[origIdx].x :
                          (axis == 1) ? ctx.centroids[origIdx].y :
                                        ctx.centroids[origIdx].z;
                int binIdx = std::min(static_cast<int>((c - minC) * scale), numBins - 1);
                bins[binIdx].count++;

                auto const & b = ctx.triangleBounds[origIdx];
                bins[binIdx].bounds.min.x = std::min(bins[binIdx].bounds.min.x, b.min.x);
                bins[binIdx].bounds.min.y = std::min(bins[binIdx].bounds.min.y, b.min.y);
                bins[binIdx].bounds.min.z = std::min(bins[binIdx].bounds.min.z, b.min.z);
                bins[binIdx].bounds.max.x = std::max(bins[binIdx].bounds.max.x, b.max.x);
                bins[binIdx].bounds.max.y = std::max(bins[binIdx].bounds.max.y, b.max.y);
                bins[binIdx].bounds.max.z = std::max(bins[binIdx].bounds.max.z, b.max.z);
            }

            // Evaluate SAH for each split
            for (int splitBin = 1; splitBin < numBins; ++splitBin)
            {
                BoundingBox leftBounds{}, rightBounds{};
                int leftCount = 0, rightCount = 0;

                for (int b = 0; b < splitBin; ++b)
                {
                    if (bins[b].count > 0)
                    {
                        leftCount += bins[b].count;
                        leftBounds.min.x = std::min(leftBounds.min.x, bins[b].bounds.min.x);
                        leftBounds.min.y = std::min(leftBounds.min.y, bins[b].bounds.min.y);
                        leftBounds.min.z = std::min(leftBounds.min.z, bins[b].bounds.min.z);
                        leftBounds.max.x = std::max(leftBounds.max.x, bins[b].bounds.max.x);
                        leftBounds.max.y = std::max(leftBounds.max.y, bins[b].bounds.max.y);
                        leftBounds.max.z = std::max(leftBounds.max.z, bins[b].bounds.max.z);
                    }
                }

                for (int b = splitBin; b < numBins; ++b)
                {
                    if (bins[b].count > 0)
                    {
                        rightCount += bins[b].count;
                        rightBounds.min.x = std::min(rightBounds.min.x, bins[b].bounds.min.x);
                        rightBounds.min.y = std::min(rightBounds.min.y, bins[b].bounds.min.y);
                        rightBounds.min.z = std::min(rightBounds.min.z, bins[b].bounds.min.z);
                        rightBounds.max.x = std::max(rightBounds.max.x, bins[b].bounds.max.x);
                        rightBounds.max.y = std::max(rightBounds.max.y, bins[b].bounds.max.y);
                        rightBounds.max.z = std::max(rightBounds.max.z, bins[b].bounds.max.z);
                    }
                }

                if (leftCount == 0 || rightCount == 0)
                {
                    continue;
                }

                float cost = params.traversalCost +
                    (surfaceArea(leftBounds) * leftCount +
                     surfaceArea(rightBounds) * rightCount) *
                    params.intersectionCost / parentArea;

                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    // Compute actual split position
                    float splitPos = minC + (maxC - minC) * splitBin / numBins;
                    (void)splitPos;  // Used for reference, actual split by sorting

                    // Track the split bin for partitioning
                    bestSplit = start + leftCount;
                }
            }
        }

        // Check if split is beneficial
        float leafCost = primCount * params.intersectionCost;
        if (bestCost >= leafCost || bestSplit == start || bestSplit == end)
        {
            // Create leaf
            node.leftChild = -1;
            node.rightChild = -1;
            node.primStart = start;
            node.primCount = primCount;
            return nodeIndex;
        }

        // Sort triangles by centroid on best axis
        std::sort(
            ctx.triangleIndices.begin() + start,
            ctx.triangleIndices.begin() + end,
            [&ctx, bestAxis](int a, int b) {
                float cA = (bestAxis == 0) ? ctx.centroids[a].x :
                           (bestAxis == 1) ? ctx.centroids[a].y :
                                             ctx.centroids[a].z;
                float cB = (bestAxis == 0) ? ctx.centroids[b].x :
                           (bestAxis == 1) ? ctx.centroids[b].y :
                                             ctx.centroids[b].z;
                return cA < cB;
            });

        // Use middle split after sorting
        int mid = start + primCount / 2;
        if (mid <= start)
        {
            mid = start + 1;
        }
        if (mid >= end)
        {
            mid = end - 1;
        }

        // Build children
        node.primStart = 0;
        node.primCount = 0;
        node.leftChild = buildRecursive(ctx, start, mid, depth + 1, params);
        node.rightChild = buildRecursive(ctx, mid, end, depth + 1, params);

        return nodeIndex;
    }

    float MeshBVHBuilder::evaluateSAH(BuildContext const & ctx,
                                      int start,
                                      int split,
                                      int end,
                                      MeshBVHBuildParams const & params)
    {
        // This method is now replaced by inline SAH in buildRecursive
        (void)ctx;
        (void)start;
        (void)split;
        (void)end;
        (void)params;
        return 0.0f;
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

}  // namespace gladius
