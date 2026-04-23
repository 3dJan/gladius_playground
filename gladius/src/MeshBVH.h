#pragma once

/// @file MeshBVH.h
/// @brief BVH builder for triangle meshes with spatial SDF queries
/// @details Implements a Bounding Volume Hierarchy using Surface Area Heuristic (SAH)
///          for fast closest-point queries on triangle meshes. Includes angle-weighted
///          vertex normals for pseudo-normal sign determination.
///
/// @see BeamBVH.h for the beam lattice BVH implementation this is based on
/// @see research.md for algorithm decisions (Bærentzen & Aanæs, 2005)

#include "kernel/types.h"

#include <memory>
#include <span>
#include <vector>

namespace gladius
{
    /// Triangle vertex indices for mesh indexing
    struct TriangleIndices
    {
        int i0;  ///< First vertex index
        int i1;  ///< Second vertex index
        int i2;  ///< Third vertex index

        TriangleIndices()
            : i0(0), i1(0), i2(0)
        {
        }

        TriangleIndices(int a, int b, int c)
            : i0(a), i1(b), i2(c)
        {
        }
    };

    /// GPU-compatible BVH node structure for triangle mesh traversal
    /// @note Size: 48 bytes (aligned to 16-byte boundary)
    /// @invariant If isLeaf(): leftChild == -1 && rightChild == -1 && primCount > 0
    /// @invariant If internal: leftChild >= 0 && rightChild >= 0 && primCount == 0
    struct MeshBVHNode
    {
        float4 bboxMin;     ///< Bounding box minimum (xyz, w unused)
        float4 bboxMax;     ///< Bounding box maximum (xyz, w unused)
        int leftChild;      ///< Index of left child (-1 if leaf)
        int rightChild;     ///< Index of right child (-1 if leaf)
        int primStart;      ///< First triangle index (leaf nodes only)
        int primCount;      ///< Number of triangles (leaf nodes only)

        MeshBVHNode()
            : bboxMin{0.f, 0.f, 0.f, 0.f}
            , bboxMax{0.f, 0.f, 0.f, 0.f}
            , leftChild(-1)
            , rightChild(-1)
            , primStart(0)
            , primCount(0)
        {
        }

        /// Check if this node is a leaf node
        /// @return true if this is a leaf node containing primitives
        bool isLeaf() const
        {
            return leftChild == -1 && rightChild == -1;
        }
    };

    /// Triangle data with vertex indices for normal lookup
    /// @note Size: 64 bytes (extended from 48 bytes for precomputed face normal)
    /// @invariant Winding order is CCW when viewed from outside
    /// @invariant vertexIndices[i] >= 0 for all i
    struct MeshTriangle
    {
        float4 v0;          ///< First vertex position (w unused)
        float4 v1;          ///< Second vertex position (w unused)
        float4 v2;          ///< Third vertex position (w unused)
        float4 faceNormal;  ///< Precomputed face normal (xyz normalized, w unused)

        MeshTriangle()
            : v0{0.f, 0.f, 0.f, 0.f}
            , v1{0.f, 0.f, 0.f, 0.f}
            , v2{0.f, 0.f, 0.f, 0.f}
            , faceNormal{0.f, 0.f, 0.f, 0.f}
        {
        }
    };

    /// Angle-weighted pseudo-normal for sign determination
    /// @note Size: 16 bytes
    /// @details Computed as sum of (angle × faceNormal) for all incident faces, normalized
    struct MeshVertexNormal
    {
        float4 normal;      ///< Angle-weighted normal (xyz normalized, w = vertex index)

        MeshVertexNormal()
            : normal{0.f, 0.f, 0.f, 0.f}
        {
        }
    };

    /// Edge pseudo-normal data for sign determination on edge features
    /// @note Size: 16 bytes per edge, 3 edges per triangle = 48 bytes per triangle
    /// @details Stores the unit face normal of the triangle adjacent across this edge.
    ///          For boundary or non-manifold edges, stored as (0, 0, 0) with w = 0.
    ///          Edge index convention matches sqTriangleWithClosestPoint in mesh_sdf.cl:
    ///            edge 0 = v0–v1, edge 1 = v1–v2, edge 2 = v0–v2.
    struct MeshEdgeNeighborNormal
    {
        float4 normal;      ///< Adjacent face unit normal (xyz); w = 1 if present, 0 if missing

        MeshEdgeNeighborNormal()
            : normal{0.f, 0.f, 0.f, 0.f}
        {
        }
    };

    /// Host-side container for mesh BVH data before serialization
    struct SpatialMeshData
    {
        std::vector<MeshBVHNode> nodes;           ///< BVH node array (root at index 0)
        std::vector<MeshTriangle> triangles;      ///< Triangle data in BVH order
        std::vector<MeshVertexNormal> vertexNormals; ///< Angle-weighted vertex normals
        std::vector<TriangleIndices> triangleIndices; ///< Vertex indices per triangle (BVH order)
        std::vector<MeshEdgeNeighborNormal> edgeNeighborNormals; ///< 3 entries per triangle (BVH order)
        size_t originalTriangleCount = 0;         ///< Source mesh triangle count
        BoundingBox boundingBox;                  ///< Axis-aligned bounding box

        /// Check if data is empty
        bool empty() const
        {
            return triangles.empty();
        }
    };

    /// Configuration parameters for BVH construction
    struct MeshBVHBuildParams
    {
        int maxDepth = 24;              ///< Maximum tree depth
        int maxPrimitivesPerLeaf = 4;   ///< Target primitives per leaf
        float traversalCost = 1.0f;     ///< SAH traversal cost
        float intersectionCost = 1.5f;  ///< SAH intersection cost
    };

    /// Statistics from BVH construction
    struct MeshBVHBuildStats
    {
        int totalNodes = 0;
        int leafNodes = 0;
        int maxDepth = 0;
        float avgPrimitivesPerLeaf = 0.0f;
        double buildTimeMs = 0.0;
    };

    /// Builds a BVH for triangle mesh closest-point queries
    /// @details Uses Surface Area Heuristic (SAH) with binned splitting,
    ///          following the BeamBVHBuilder pattern.
    class MeshBVHBuilder
    {
      public:
        MeshBVHBuilder() = default;
        ~MeshBVHBuilder() = default;

        /// Build BVH from mesh data
        /// @param vertices Vertex positions (float4, xyz used, w ignored)
        /// @param indices Triangle indices (3 indices per triangle)
        /// @param params Build configuration
        /// @return SpatialMeshData containing BVH, triangles, and normals
        /// @note Returns empty SpatialMeshData for empty input (does not throw)
        /// @note Complexity: O(n log n) for n triangles
        SpatialMeshData build(std::span<float4 const> vertices,
                              std::span<TriangleIndices const> indices,
                              MeshBVHBuildParams const & params = {});

        /// Get statistics from last build
        MeshBVHBuildStats const & getLastBuildStats() const
        {
            return m_lastStats;
        }

      private:
        MeshBVHBuildStats m_lastStats;

        /// Internal build context
        struct BuildContext;

        /// Compute angle-weighted vertex normals
        void computeAngleWeightedNormals(std::span<float4 const> vertices,
                                         std::span<TriangleIndices const> indices,
                                         SpatialMeshData & data);

        /// Compute the unit face normal of the triangle adjacent across each edge of
        /// every triangle. Result is written into data.edgeNeighborNormals in BVH order.
        /// Boundary or non-manifold edges yield a zero vector with w = 0.
        void computeEdgeNeighborNormals(std::span<float4 const> vertices,
                                        std::span<TriangleIndices const> originalIndices,
                                        std::vector<int> const & bvhToOriginalTriangle,
                                        SpatialMeshData & data);

        /// Compute AABB for a triangle
        BoundingBox computeTriangleBounds(MeshTriangle const & tri);

        /// Recursive BVH construction using SAH
        /// @param ctx Build context (mutable: nodes are appended, indices partitioned in place)
        /// @param start First primitive index in this node's range (inclusive)
        /// @param end Last primitive index in this node's range (exclusive)
        /// @param depth Current recursion depth
        /// @param params Build configuration
        /// @param nodeBounds Pre-computed AABB enclosing this node's primitives (passed down
        ///                   from the parent's split sweep to avoid recomputing)
        /// @param centroidBounds Pre-computed AABB of this node's primitive centroids
        ///                       (drives axis selection without an extra sweep)
        int buildRecursive(BuildContext & ctx,
                           int start,
                           int end,
                           int depth,
                           MeshBVHBuildParams const & params,
                           BoundingBox const & nodeBounds,
                           BoundingBox const & centroidBounds);

        /// Calculate surface area of bounding box
        static float surfaceArea(BoundingBox const & box);
    };
}
