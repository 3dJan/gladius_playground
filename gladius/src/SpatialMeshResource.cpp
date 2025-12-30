/// @file SpatialMeshResource.cpp
/// @brief Implementation of SpatialMeshResource for mesh SDF computation
/// @see SpatialMeshResource.h

#include "SpatialMeshResource.h"

#include <cstring>

namespace gladius
{
    namespace
    {
        /// Convert int to float preserving bit pattern (for GPU interop)
        inline float intBitsToFloat(int value)
        {
            float result;
            std::memcpy(&result, &value, sizeof(float));
            return result;
        }
    }  // namespace
    // ========================================================================
    // Constructors
    // ========================================================================

    SpatialMeshResource::SpatialMeshResource(ResourceKey key, SpatialMeshData && data)
        : ResourceBase(std::move(key))
        , m_data(std::move(data))
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             std::span<float4 const> vertices,
                                             std::span<TriangleIndices const> indices)
        : ResourceBase(std::move(key))
    {
        MeshBVHBuilder builder;
        MeshBVHBuildParams params;
        params.maxPrimitivesPerLeaf = 4;
        params.maxDepth = 32;
        params.traversalCost = 1.0f;
        params.intersectionCost = 1.0f;

        m_data = builder.build(vertices, indices, params);
        ResourceBase::load();
    }

    // ========================================================================
    // Public Methods
    // ========================================================================

    void SpatialMeshResource::invalidate()
    {
        m_needsRebuild = true;
    }

    void SpatialMeshResource::rebuild(std::span<float4 const> vertices,
                                      std::span<TriangleIndices const> indices)
    {
        MeshBVHBuilder builder;
        MeshBVHBuildParams params;
        params.maxPrimitivesPerLeaf = 4;
        params.maxDepth = 32;
        params.traversalCost = 1.0f;
        params.intersectionCost = 1.0f;

        m_data = builder.build(vertices, indices, params);
        m_needsRebuild = false;

        // Clear and reload payload data
        m_payloadData.data.clear();
        m_payloadData.meta.clear();
        loadImpl();
    }

    // ========================================================================
    // ResourceBase Interface
    // ========================================================================

    void SpatialMeshResource::loadImpl()
    {
        if (m_data.empty())
        {
            return;
        }

        // Clear previous payload
        m_payloadData.meta.clear();

        // Create primitive metadata for the spatial mesh root
        PrimitiveMeta metaData{};
        metaData.primitiveType = SDF_SPATIAL_MESH_ROOT;
        metaData.start = static_cast<int>(m_payloadData.data.size());

        // Serialize bounding box (2 float4s = 8 floats)
        m_payloadData.data.push_back(m_data.boundingBox.min.x);
        m_payloadData.data.push_back(m_data.boundingBox.min.y);
        m_payloadData.data.push_back(m_data.boundingBox.min.z);
        m_payloadData.data.push_back(m_data.boundingBox.min.w);
        m_payloadData.data.push_back(m_data.boundingBox.max.x);
        m_payloadData.data.push_back(m_data.boundingBox.max.y);
        m_payloadData.data.push_back(m_data.boundingBox.max.z);
        m_payloadData.data.push_back(m_data.boundingBox.max.w);

        // Serialize counts (4 floats)
        m_payloadData.data.push_back(static_cast<float>(m_data.nodes.size()));
        m_payloadData.data.push_back(static_cast<float>(m_data.triangles.size()));
        m_payloadData.data.push_back(static_cast<float>(m_data.vertexNormals.size()));
        m_payloadData.data.push_back(0.0f);  // Reserved

        // Record where node data starts
        int nodeDataStart = static_cast<int>(m_payloadData.data.size());
        m_payloadData.data.push_back(static_cast<float>(nodeDataStart));
        m_payloadData.data.push_back(0.0f);  // Triangle data offset (computed later)
        m_payloadData.data.push_back(0.0f);  // Normal data offset (computed later)
        m_payloadData.data.push_back(0.0f);  // Reserved

        // Serialize BVH nodes
        // Each node: bboxMin (4), bboxMax (4), leftChild, rightChild, primStart, primCount = 12 floats
        // Integer fields are bit-cast to preserve their bit patterns for GPU struct interpretation
        size_t actualNodeStart = m_payloadData.data.size();
        for (auto const & node : m_data.nodes)
        {
            m_payloadData.data.push_back(node.bboxMin.x);
            m_payloadData.data.push_back(node.bboxMin.y);
            m_payloadData.data.push_back(node.bboxMin.z);
            m_payloadData.data.push_back(node.bboxMin.w);
            m_payloadData.data.push_back(node.bboxMax.x);
            m_payloadData.data.push_back(node.bboxMax.y);
            m_payloadData.data.push_back(node.bboxMax.z);
            m_payloadData.data.push_back(node.bboxMax.w);
            m_payloadData.data.push_back(intBitsToFloat(node.leftChild));
            m_payloadData.data.push_back(intBitsToFloat(node.rightChild));
            m_payloadData.data.push_back(intBitsToFloat(node.primStart));
            m_payloadData.data.push_back(intBitsToFloat(node.primCount));
        }

        // Serialize triangles
        // Each triangle: v0 (4), v1 (4), v2 (4) = 12 floats
        size_t triDataStart = m_payloadData.data.size();
        for (auto const & tri : m_data.triangles)
        {
            m_payloadData.data.push_back(tri.v0.x);
            m_payloadData.data.push_back(tri.v0.y);
            m_payloadData.data.push_back(tri.v0.z);
            m_payloadData.data.push_back(tri.v0.w);
            m_payloadData.data.push_back(tri.v1.x);
            m_payloadData.data.push_back(tri.v1.y);
            m_payloadData.data.push_back(tri.v1.z);
            m_payloadData.data.push_back(tri.v1.w);
            m_payloadData.data.push_back(tri.v2.x);
            m_payloadData.data.push_back(tri.v2.y);
            m_payloadData.data.push_back(tri.v2.z);
            m_payloadData.data.push_back(tri.v2.w);
        }

        // Serialize vertex normals
        // Each normal: xyz + w (vertex index) = 4 floats
        size_t normalDataStart = m_payloadData.data.size();
        for (auto const & vn : m_data.vertexNormals)
        {
            m_payloadData.data.push_back(vn.normal.x);
            m_payloadData.data.push_back(vn.normal.y);
            m_payloadData.data.push_back(vn.normal.z);
            m_payloadData.data.push_back(vn.normal.w);
        }

        // Serialize triangle indices for normal lookup
        // Each triangle: 3 vertex indices = 4 ints (bit-cast to preserve int bit patterns)
        for (auto const & idx : m_data.triangleIndices)
        {
            m_payloadData.data.push_back(intBitsToFloat(idx.i0));
            m_payloadData.data.push_back(intBitsToFloat(idx.i1));
            m_payloadData.data.push_back(intBitsToFloat(idx.i2));
            m_payloadData.data.push_back(0.0f);  // Padding for alignment
        }

        // Patch offsets in header
        m_payloadData.data[nodeDataStart] = static_cast<float>(actualNodeStart);
        m_payloadData.data[nodeDataStart + 1] = static_cast<float>(triDataStart);
        m_payloadData.data[nodeDataStart + 2] = static_cast<float>(normalDataStart);

        metaData.end = static_cast<int>(m_payloadData.data.size());
        m_payloadData.meta.push_back(metaData);
    }

}  // namespace gladius
