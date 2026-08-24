#pragma once

/// @file MeshPayloadSerializer.h
/// @brief Serializes SpatialMeshData into the backend-neutral flat float payload
///        used by both the OpenCL primitives buffer and WebGPU storage buffers.
///
/// The produced layout matches SpatialMeshResource::loadImpl() for the pure-BVH
/// method: a 34-float header followed by BVH nodes, triangles, vertex normals,
/// triangle indices, and per-edge adjacent face normals. All offsets in the
/// header are local to the payload start, so the payload can be uploaded as a
/// standalone GPU buffer without further patching.
///
/// Acceleration structures that require GPU-side post-upload builds on OpenCL
/// (voxel grid, FWN aggregates, sign cache, NanoVDB) are intentionally not
/// serialized here; the corresponding header slots report "not available" so
/// kernels fall back to pure-BVH evaluation.

#include "MeshBVH.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace gladius::io
{
    /// Number of floats in the spatial-mesh payload header.
    inline constexpr std::size_t MESH_PAYLOAD_HEADER_FLOATS = 34;

    /// Header slot indices (see SpatialMeshResource.cpp for the full layout).
    inline constexpr std::size_t MESH_PAYLOAD_BVH_OFFSETS_INDEX = 12;
    inline constexpr std::size_t MESH_PAYLOAD_EDGE_NEIGHBORS_OFFSET_INDEX = 28;
    inline constexpr std::size_t MESH_PAYLOAD_FWN_AGGREGATES_OFFSET_INDEX = 29;
    inline constexpr std::size_t MESH_PAYLOAD_SIGN_CACHE_OFFSET_INDEX = 30;
    inline constexpr std::size_t MESH_PAYLOAD_NANOVDB_OFFSET_INDEX = 33;

    /// Convert int to float preserving bit pattern (GPU interop).
    inline float meshPayloadIntBitsToFloat(int value)
    {
        float result;
        std::memcpy(&result, &value, sizeof(float));
        return result;
    }

    inline void padMeshPayloadToFloatAlignment(std::vector<float> & data,
                                               std::size_t alignmentFloats)
    {
        if (alignmentFloats == 0u)
        {
            return;
        }
        auto const padding = (alignmentFloats - (data.size() % alignmentFloats)) % alignmentFloats;
        data.insert(data.end(), padding, 0.0f);
    }

    /**
     * @brief Serialize pre-built BVH mesh data into the flat float payload.
     *
     * Produces the pure-BVH payload: header + nodes + triangles + vertex normals +
     * triangle indices + edge-neighbor normals. Voxel/FWN/NanoVDB sections are
     * omitted and their header slots are zeroed ("not available").
     *
     * @param data Pre-built BVH and mesh data (as produced by MeshBVHBuilder).
     * @return Flat float array with local offsets, ready for GPU upload.
     */
    [[nodiscard]] inline std::vector<float> serializeSpatialMeshPayload(
      SpatialMeshData const & data)
    {
        std::vector<float> out;
        out.reserve(MESH_PAYLOAD_HEADER_FLOATS + data.nodes.size() * 12u +
                    data.triangles.size() * 16u + data.vertexNormals.size() * 4u +
                    data.triangleIndices.size() * 4u + data.edgeNeighborNormals.size() * 4u);

        // [0-7]: bounding box (min.xyzw, max.xyzw)
        out.push_back(data.boundingBox.min.x);
        out.push_back(data.boundingBox.min.y);
        out.push_back(data.boundingBox.min.z);
        out.push_back(data.boundingBox.min.w);
        out.push_back(data.boundingBox.max.x);
        out.push_back(data.boundingBox.max.y);
        out.push_back(data.boundingBox.max.z);
        out.push_back(data.boundingBox.max.w);

        // [8-11]: counts (nodeCount, triCount, vertexNormalCount, reserved)
        out.push_back(static_cast<float>(data.nodes.size()));
        out.push_back(static_cast<float>(data.triangles.size()));
        out.push_back(static_cast<float>(data.vertexNormals.size()));
        out.push_back(0.0f);

        // [12-15]: BVH offsets placeholder (patched below)
        std::size_t const bvhOffsetsIndex = out.size();
        out.insert(out.end(), 4u, 0.0f);

        // [16-25]: voxel grid header (all zeros = not populated)
        out.insert(out.end(), 10u, 0.0f);

        // [26-27]: voxel info (offset 0, count 0 → kernels use pure-BVH path)
        out.push_back(0.0f);
        out.push_back(0.0f);

        // [28]: edge-neighbour normals offset (patched below)
        std::size_t const edgeNeighborsIndex = out.size();
        out.push_back(0.0f);

        // [29]: FWN aggregate offset (0 = not built)
        out.push_back(0.0f);
        // [30]: FWN sign-cache offset (0 = not ready)
        out.push_back(0.0f);
        // [31]: sign-cache resolution (informational)
        out.push_back(64.0f);
        // [32]: FWN beta (unused without aggregates)
        out.push_back(0.0f);
        // [33]: NanoVDB grid offset (0 = not built)
        out.push_back(0.0f);

        // BVH nodes: bboxMin(4), bboxMax(4), leftChild, rightChild, primStart, primCount
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const nodesOffset = out.size();
        for (auto const & node : data.nodes)
        {
            out.push_back(node.bboxMin.x);
            out.push_back(node.bboxMin.y);
            out.push_back(node.bboxMin.z);
            out.push_back(node.bboxMin.w);
            out.push_back(node.bboxMax.x);
            out.push_back(node.bboxMax.y);
            out.push_back(node.bboxMax.z);
            out.push_back(node.bboxMax.w);
            out.push_back(meshPayloadIntBitsToFloat(node.leftChild));
            out.push_back(meshPayloadIntBitsToFloat(node.rightChild));
            out.push_back(meshPayloadIntBitsToFloat(node.primStart));
            out.push_back(meshPayloadIntBitsToFloat(node.primCount));
        }

        // Triangles: v0(4), v1(4), v2(4), faceNormal(4)
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const trianglesOffset = out.size();
        for (auto const & tri : data.triangles)
        {
            out.push_back(tri.v0.x);
            out.push_back(tri.v0.y);
            out.push_back(tri.v0.z);
            out.push_back(tri.v0.w);
            out.push_back(tri.v1.x);
            out.push_back(tri.v1.y);
            out.push_back(tri.v1.z);
            out.push_back(tri.v1.w);
            out.push_back(tri.v2.x);
            out.push_back(tri.v2.y);
            out.push_back(tri.v2.z);
            out.push_back(tri.v2.w);
            out.push_back(tri.faceNormal.x);
            out.push_back(tri.faceNormal.y);
            out.push_back(tri.faceNormal.z);
            out.push_back(tri.faceNormal.w);
        }

        // Vertex normals: xyz + w(vertex index)
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const normalsOffset = out.size();
        for (auto const & vn : data.vertexNormals)
        {
            out.push_back(vn.normal.x);
            out.push_back(vn.normal.y);
            out.push_back(vn.normal.z);
            out.push_back(vn.normal.w);
        }

        // Triangle indices: i0, i1, i2, padding
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const indicesOffset = out.size();
        for (auto const & idx : data.triangleIndices)
        {
            out.push_back(meshPayloadIntBitsToFloat(idx.i0));
            out.push_back(meshPayloadIntBitsToFloat(idx.i1));
            out.push_back(meshPayloadIntBitsToFloat(idx.i2));
            out.push_back(0.0f);
        }

        // Per-edge adjacent face normals: 3 entries per triangle
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const edgeNeighborsOffset = out.size();
        for (auto const & en : data.edgeNeighborNormals)
        {
            out.push_back(en.normal.x);
            out.push_back(en.normal.y);
            out.push_back(en.normal.z);
            out.push_back(en.normal.w);
        }

        // Patch header offsets (local).
        out[bvhOffsetsIndex + 0] = static_cast<float>(nodesOffset);
        out[bvhOffsetsIndex + 1] = static_cast<float>(trianglesOffset);
        out[bvhOffsetsIndex + 2] = static_cast<float>(normalsOffset);
        out[bvhOffsetsIndex + 3] = static_cast<float>(indicesOffset);
        out[edgeNeighborsIndex] = static_cast<float>(edgeNeighborsOffset);

        return out;
    }
}
