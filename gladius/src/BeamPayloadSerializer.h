#pragma once

/// @file BeamPayloadSerializer.h
/// @brief Serializes beam lattice data into the backend-neutral flat float
///        payload used by both the OpenCL primitives buffer and WebGPU storage
///        buffers.
///
/// The produced layout mirrors BeamLatticeResource::buildBVH() payload sections,
/// but with a self-contained header so the payload can be uploaded as a
/// standalone GPU buffer with local offsets (no PrimitiveMeta indirection):
///
///   Header (8 floats):
///     [0] bvhNodesOffset      [1] primitiveIndicesOffset
///     [2] beamsOffset         [3] ballsOffset
///     [4] bvhNodeCount        [5] beamCount
///     [6] ballCount           [7] reserved (0)
///
///   BVH node (10 floats, matching OpenCL evaluateBeamLatticeBVH):
///     bbMin.xyz(3), bbMax.xyz(3), leftChild, rightChild, primStart, primCount
///     (ints stored as plain float values; -1.0f marks "no child")
///
///   Primitive index entry (3 floats): type (0=beam, 1=ball), index, unused
///   Beam (11 floats): startPos.xyz, endPos.xyz, startRadius, endRadius,
///                     startCapStyle, endCapStyle, materialId
///   Ball (4 floats): position.xyz, radius

#include "BeamBVH.h"
#include "MeshPayloadSerializer.h"
#include "kernel/types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gladius::io
{
    /// Number of floats in the beam lattice payload header.
    inline constexpr std::size_t BEAM_PAYLOAD_HEADER_FLOATS = 8u;

    /**
     * @brief Serialize beam lattice data into a self-contained flat float payload.
     *
     * Rebuilds the BVH from the given beams/balls (matching the resource's own
     * build) so the payload carries consistent node/primitive ordering.
     *
     * @param beams Beam primitives.
     * @param balls Ball primitives.
     * @return Flat float array with local offsets, ready for GPU upload.
     */
    [[nodiscard]] inline std::vector<float> serializeBeamLatticePayload(
      std::vector<BeamData> const & beams, std::vector<BallData> const & balls)
    {
        BeamBVHBuilder builder;
        auto const bvhNodes = builder.build(beams, balls);
        auto const & primitiveOrdering = builder.getPrimitiveOrdering();

        std::vector<float> out;
        out.reserve(BEAM_PAYLOAD_HEADER_FLOATS + bvhNodes.size() * 10u +
                    primitiveOrdering.size() * 3u + beams.size() * 11u + balls.size() * 4u);

        // Header placeholder (patched below).
        std::size_t const headerIndex = out.size();
        out.insert(out.end(), BEAM_PAYLOAD_HEADER_FLOATS, 0.0f);

        // BVH nodes: bbMin.xyz(3), bbMax.xyz(3), leftChild, rightChild,
        // primStart, primCount — ints stored as plain float values.
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const bvhOffset = out.size();
        for (auto const & node : bvhNodes)
        {
            out.push_back(node.boundingBox.min.x);
            out.push_back(node.boundingBox.min.y);
            out.push_back(node.boundingBox.min.z);
            out.push_back(node.boundingBox.max.x);
            out.push_back(node.boundingBox.max.y);
            out.push_back(node.boundingBox.max.z);
            out.push_back(static_cast<float>(node.leftChild));
            out.push_back(static_cast<float>(node.rightChild));
            out.push_back(static_cast<float>(node.primitiveStart));
            out.push_back(static_cast<float>(node.primitiveCount));
        }

        // Primitive index entries: type (0=beam, 1=ball), index, unused.
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const indicesOffset = out.size();
        for (auto const & primitive : primitiveOrdering)
        {
            out.push_back(static_cast<float>(primitive.type == BeamPrimitive::BEAM ? 0 : 1));
            out.push_back(static_cast<float>(primitive.index));
            out.push_back(0.0f);
        }

        // Beams: startPos.xyz(3), endPos.xyz(3), radii(2), caps(2), material(1).
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const beamsOffset = out.size();
        for (auto const & beam : beams)
        {
            out.push_back(beam.startPos.x);
            out.push_back(beam.startPos.y);
            out.push_back(beam.startPos.z);
            out.push_back(beam.endPos.x);
            out.push_back(beam.endPos.y);
            out.push_back(beam.endPos.z);
            out.push_back(beam.startRadius);
            out.push_back(beam.endRadius);
            out.push_back(static_cast<float>(beam.startCapStyle));
            out.push_back(static_cast<float>(beam.endCapStyle));
            out.push_back(static_cast<float>(beam.materialId));
        }

        // Balls: position.xyz + radius.
        padMeshPayloadToFloatAlignment(out, 4u);
        std::size_t const ballsOffset = out.size();
        for (auto const & ball : balls)
        {
            out.push_back(ball.positionRadius.x);
            out.push_back(ball.positionRadius.y);
            out.push_back(ball.positionRadius.z);
            out.push_back(ball.positionRadius.w);
        }

        // Patch header.
        out[headerIndex + 0u] = static_cast<float>(bvhOffset);
        out[headerIndex + 1u] = static_cast<float>(indicesOffset);
        out[headerIndex + 2u] = static_cast<float>(beamsOffset);
        out[headerIndex + 3u] = static_cast<float>(ballsOffset);
        out[headerIndex + 4u] = static_cast<float>(bvhNodes.size());
        out[headerIndex + 5u] = static_cast<float>(beams.size());
        out[headerIndex + 6u] = static_cast<float>(balls.size());

        return out;
    }
}
