#pragma once

/// @file MeshSdfReference.h
/// @brief Brute-force host-side reference SDF for triangle meshes.
///
/// Provides ground-truth implementations used by tests and benchmarks
/// to verify the GPU mesh SDF kernels. None of these functions are
/// optimised — they iterate over every triangle for every query —
/// they exist purely to be obviously correct.
///
/// Two independent quantities are provided:
///   * unsigned closest-point distance (Euclidean),
///   * generalised winding number (Van Oosterom & Strang 1983) which
///     is positive inside a closed surface and ~0 outside, regardless
///     of triangle orientation consistency.
///
/// `referenceSignedDistance` combines them: distance is negated when
/// the winding number indicates "inside" (w > 0.5).

#include "MeshBVH.h"

#include <span>
#include <vector>

namespace gladius::mesh_sdf_reference
{
    /// Unsigned Euclidean closest-point distance from @p query to the
    /// triangle mesh defined by (@p vertices, @p indices). O(N) per call.
    float referenceUnsignedDistance(std::span<float4 const> vertices,
                                    std::span<TriangleIndices const> indices,
                                    float4 const & query);

    /// Generalised winding number of the mesh at @p query using the exact
    /// per-triangle solid-angle formula (Van Oosterom-Strang). Returns ~1
    /// inside a closed manifold surface and ~0 outside; the value is
    /// well-defined and continuous for non-closed and self-intersecting
    /// inputs. O(N) per call.
    float referenceWindingNumber(std::span<float4 const> vertices,
                                 std::span<TriangleIndices const> indices,
                                 float4 const & query);

    /// Signed distance: unsigned closest-point distance with sign chosen
    /// from the winding number (negative when @c referenceWindingNumber
    /// returns > 0.5). O(N) per call.
    float referenceSignedDistance(std::span<float4 const> vertices,
                                  std::span<TriangleIndices const> indices,
                                  float4 const & query);

} // namespace gladius::mesh_sdf_reference
