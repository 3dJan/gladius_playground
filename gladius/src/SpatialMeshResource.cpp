/// @file SpatialMeshResource.cpp
/// @brief Implementation of SpatialMeshResource for mesh SDF computation
/// @see SpatialMeshResource.h

#include "SpatialMeshResource.h"
#include "MeshVoxelGrid.h"

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
        : MeshResourceBase(std::move(key))
        , m_data(std::move(data))
    {
        ResourceBase::load();
    }

    SpatialMeshResource::SpatialMeshResource(ResourceKey key,
                                             std::span<float4 const> vertices,
                                             std::span<TriangleIndices const> indices)
        : MeshResourceBase(std::move(key))
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
        m_needsVoxelGridBuild = true;
    }

    bool SpatialMeshResource::setEvaluationConfig(MeshSdfEvaluationConfig const & cfg)
    {
        bool const rebuildRequired = requiresMeshRebuild(m_evaluationConfig, cfg);
        m_evaluationConfig = cfg;
        if (rebuildRequired)
        {
            // Drop the cached payload and re-serialise with the new method
            // (e.g. allocating or skipping the voxel grid). load() is a no-op
            // after the first call, so call loadImpl() directly here, mirroring
            // what rebuild() does.
            m_payloadData.data.clear();
            m_payloadData.meta.clear();
            m_needsRebuild = true;
            m_needsVoxelGridBuild = (cfg.method == MeshSdfMethod::VoxelAccelerated);
            loadImpl();
        }
        return rebuildRequired;
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
        m_needsVoxelGridBuild = true;

        // Clear and reload payload data
        m_payloadData.data.clear();
        m_payloadData.meta.clear();
        loadImpl();
    }
    
    // ========================================================================
    // Header Layout Constants
    // ========================================================================
    static constexpr size_t kBboxFloats = 8;           // min.xyzw + max.xyzw
    static constexpr size_t kCountsFloats = 4;         // nodeCount, triCount, vertexNormalCount, reserved
    static constexpr size_t kBvhOffsetsFloats = 4;     // nodesOffset, trianglesOffset, normalsOffset, indicesOffset
    static constexpr size_t kVoxelHeaderFloats = 10;   // origin.xyz, dims.xyz, voxelSize, invVoxelSize, threshold, padding
    static constexpr size_t kVoxelInfoFloats = 2;      // voxelDataOffset, voxelCount
    static constexpr size_t kReservedFloats = 4;       // [edgeNeighborsOffset, reserved, reserved, reserved]
    
    static constexpr size_t kBvhOffsetsOffset = kBboxFloats + kCountsFloats;  // 12
    static constexpr size_t kVoxelInfoOffset = kBvhOffsetsOffset + kBvhOffsetsFloats + kVoxelHeaderFloats;  // 26
    static constexpr size_t kEdgeNeighborsOffsetIndex = kVoxelInfoOffset + kVoxelInfoFloats;                // 28
    
    void SpatialMeshResource::write(Primitives & primitives)
    {
        // Track the base offset before adding our data
        m_dataBaseOffset = static_cast<int>(primitives.data.getSize());
        
        // Patch the offsets in the header to be absolute
        // These were stored as local offsets during loadImpl()
        size_t const bvhOffsetsIndex = m_headerStart + kBvhOffsetsOffset;
        size_t const voxelInfoIndex = m_headerStart + kVoxelInfoOffset;
        
        m_payloadData.data[bvhOffsetsIndex + 0] = static_cast<float>(m_dataBaseOffset + m_nodesOffset);
        m_payloadData.data[bvhOffsetsIndex + 1] = static_cast<float>(m_dataBaseOffset + m_trianglesOffset);
        m_payloadData.data[bvhOffsetsIndex + 2] = static_cast<float>(m_dataBaseOffset + m_normalsOffset);
        m_payloadData.data[bvhOffsetsIndex + 3] = static_cast<float>(m_dataBaseOffset + m_indicesOffset);
        m_payloadData.data[voxelInfoIndex] = static_cast<float>(m_dataBaseOffset + m_voxelDataOffset);
        m_payloadData.data[m_headerStart + kEdgeNeighborsOffsetIndex] =
            static_cast<float>(m_dataBaseOffset + m_edgeNeighborsOffset);
        
        // Call base implementation to add data to primitives
        ResourceBase::write(primitives);
        
        // Flag that we need a voxel grid build after upload
        m_needsVoxelGridBuild = true;
    }
    
    std::optional<MeshVoxelGridBuildParams> SpatialMeshResource::getVoxelGridBuildParams() const
    {
        if (m_data.empty() || m_voxelCount == 0)
        {
            return std::nullopt;
        }
        
        MeshVoxelGridBuildParams params{};
        params.headerStart = m_dataBaseOffset + static_cast<int>(m_headerStart);
        params.voxelDataOffset = m_dataBaseOffset + static_cast<int>(m_voxelDataOffset);
        params.nodesOffset = m_dataBaseOffset + static_cast<int>(m_nodesOffset);
        params.trianglesOffset = m_dataBaseOffset + static_cast<int>(m_trianglesOffset);
        params.normalsOffset = m_dataBaseOffset + static_cast<int>(m_normalsOffset);
        params.indicesOffset = m_dataBaseOffset + static_cast<int>(m_indicesOffset);
        params.edgeNeighborsOffset = m_dataBaseOffset + static_cast<int>(m_edgeNeighborsOffset);
        params.nodeCount = static_cast<int>(m_data.nodes.size());
        params.triCount = static_cast<int>(m_data.triangles.size());
        params.vertexNormalCount = static_cast<int>(m_data.vertexNormals.size());
        params.voxelCount = static_cast<int>(m_voxelCount);
        
        return params;
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

        // ====================================================================
        // Header Layout (32 floats total):
        // [0-7]:   Bounding box (8 floats: min.xyzw, max.xyzw)
        // [8-11]:  Counts (4 floats: nodeCount, triCount, vertexNormalCount, reserved)
        // [12-15]: BVH offsets (4 floats: nodesOffset, trianglesOffset, normalsOffset, indicesOffset)
        // [16-25]: Voxel grid header (10 floats: origin.xyz, dims.xyz, voxelSize, invVoxelSize, threshold, padding)
        // [26-27]: Voxel grid info (2 floats: voxelDataOffset, voxelCount)
        // [28-31]: Reserved (4 floats)
        // ====================================================================
        
        m_headerStart = m_payloadData.data.size();

        // Serialize bounding box (8 floats)
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

        // Placeholder for BVH offsets (4 floats) - will be patched later
        size_t const bvhOffsetsIndex = m_payloadData.data.size();
        m_payloadData.data.push_back(0.0f);  // nodesOffset
        m_payloadData.data.push_back(0.0f);  // trianglesOffset
        m_payloadData.data.push_back(0.0f);  // normalsOffset
        m_payloadData.data.push_back(0.0f);  // indicesOffset

        // Compute voxel grid header from bounding box. Resolution and whether the
        // grid is actually populated are governed by the active evaluation config.
        bool const useVoxelGrid =
            (m_evaluationConfig.method == MeshSdfMethod::VoxelAccelerated);
        int const voxelResolution = (m_evaluationConfig.voxelGridResolution > 0)
                                        ? m_evaluationConfig.voxelGridResolution
                                        : kDefaultVoxelGridResolution;
        MeshVoxelGridHeader const voxelHeader = createVoxelGridHeader(
            m_data.boundingBox.min.x, m_data.boundingBox.min.y, m_data.boundingBox.min.z,
            m_data.boundingBox.max.x, m_data.boundingBox.max.y, m_data.boundingBox.max.z,
            voxelResolution);
        
        // Serialize voxel grid header (10 floats)
        m_payloadData.data.push_back(voxelHeader.originX);
        m_payloadData.data.push_back(voxelHeader.originY);
        m_payloadData.data.push_back(voxelHeader.originZ);
        m_payloadData.data.push_back(voxelHeader.dimX);
        m_payloadData.data.push_back(voxelHeader.dimY);
        m_payloadData.data.push_back(voxelHeader.dimZ);
        m_payloadData.data.push_back(voxelHeader.voxelSize);
        m_payloadData.data.push_back(voxelHeader.invVoxelSize);
        m_payloadData.data.push_back(voxelHeader.threshold);
        m_payloadData.data.push_back(voxelHeader.padding);

        // Placeholder for voxel grid info (2 floats) - will be patched later
        size_t const voxelInfoIndex = m_payloadData.data.size();
        m_payloadData.data.push_back(0.0f);  // voxelDataOffset
        // When the chosen method does not need a voxel grid, report zero voxels
        // so the kernel dispatch in sdf.cl falls through to pure-BVH evaluation.
        m_voxelCount = useVoxelGrid ? computeVoxelCount(voxelHeader) : 0u;
        m_payloadData.data.push_back(static_cast<float>(m_voxelCount));

        // Reserved (4 floats)
        m_payloadData.data.push_back(0.0f);
        m_payloadData.data.push_back(0.0f);
        m_payloadData.data.push_back(0.0f);
        m_payloadData.data.push_back(0.0f);

        // Serialize BVH nodes
        // Each node: bboxMin (4), bboxMax (4), leftChild, rightChild, primStart, primCount = 12 floats
        m_nodesOffset = m_payloadData.data.size();
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
        // Each triangle: v0 (4), v1 (4), v2 (4), faceNormal (4) = 16 floats
        m_trianglesOffset = m_payloadData.data.size();
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
            m_payloadData.data.push_back(tri.faceNormal.x);
            m_payloadData.data.push_back(tri.faceNormal.y);
            m_payloadData.data.push_back(tri.faceNormal.z);
            m_payloadData.data.push_back(tri.faceNormal.w);
        }

        // Serialize vertex normals
        // Each normal: xyz + w (vertex index) = 4 floats
        m_normalsOffset = m_payloadData.data.size();
        for (auto const & vn : m_data.vertexNormals)
        {
            m_payloadData.data.push_back(vn.normal.x);
            m_payloadData.data.push_back(vn.normal.y);
            m_payloadData.data.push_back(vn.normal.z);
            m_payloadData.data.push_back(vn.normal.w);
        }

        // Serialize triangle indices for normal lookup
        // Each triangle: 3 vertex indices = 4 ints (padded)
        m_indicesOffset = m_payloadData.data.size();
        for (auto const & idx : m_data.triangleIndices)
        {
            m_payloadData.data.push_back(intBitsToFloat(idx.i0));
            m_payloadData.data.push_back(intBitsToFloat(idx.i1));
            m_payloadData.data.push_back(intBitsToFloat(idx.i2));
            m_payloadData.data.push_back(0.0f);  // Padding for alignment
        }

        // Serialize per-edge adjacent face normals (3 entries per triangle, 4 floats each).
        // Used by computePseudoNormalFast in mesh_sdf.cl for robust sign on edge features.
        m_edgeNeighborsOffset = m_payloadData.data.size();
        for (auto const & en : m_data.edgeNeighborNormals)
        {
            m_payloadData.data.push_back(en.normal.x);
            m_payloadData.data.push_back(en.normal.y);
            m_payloadData.data.push_back(en.normal.z);
            m_payloadData.data.push_back(en.normal.w);
        }

        // Reserve space for voxel grid data (2 floats per voxel: nearestTriIdx, signedDist)
        // This space will be filled by the buildMeshVoxelGrid kernel on GPU
        m_voxelDataOffset = m_payloadData.data.size();
        size_t const voxelDataSize = m_voxelCount * 2;  // 2 floats per voxel
        for (size_t i = 0; i < voxelDataSize; ++i)
        {
            m_payloadData.data.push_back(0.0f);  // Will be filled by GPU kernel
        }

        // Patch offsets in header (local offsets, will be adjusted with base offset when read)
        m_payloadData.data[bvhOffsetsIndex + 0] = static_cast<float>(m_nodesOffset);
        m_payloadData.data[bvhOffsetsIndex + 1] = static_cast<float>(m_trianglesOffset);
        m_payloadData.data[bvhOffsetsIndex + 2] = static_cast<float>(m_normalsOffset);
        m_payloadData.data[bvhOffsetsIndex + 3] = static_cast<float>(m_indicesOffset);
        m_payloadData.data[voxelInfoIndex] = static_cast<float>(m_voxelDataOffset);
        m_payloadData.data[m_headerStart + kEdgeNeighborsOffsetIndex] = static_cast<float>(m_edgeNeighborsOffset);

        metaData.end = static_cast<int>(m_payloadData.data.size());
        m_payloadData.meta.push_back(metaData);
    }

}  // namespace gladius
