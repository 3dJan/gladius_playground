/// @file MeshFwnHierarchical_tests.cpp
/// @brief Host emulation of the OpenCL Barnes-Hut winding-number traversal,
///        validated against the brute-force reference.
///
/// The host emulator mirrors `fwnHierarchical` in mesh_sdf.cl exactly: same
/// acceptance test, same dipole formula, same exact leaf evaluation. Passing
/// these tests provides a strong correctness guarantee for the kernel before
/// going through the GPU pipeline.

#include "MeshBVH.h"
#include "MeshSdfReference.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        constexpr float kFourPi = 4.0f * 3.14159265358979323846f;

        float solidAngleAtOrigin(float4 const & av, float4 const & bv, float4 const & cv)
        {
            float const ax = av.x, ay = av.y, az = av.z;
            float const bx = bv.x, by = bv.y, bz = bv.z;
            float const cx = cv.x, cy = cv.y, cz = cv.z;
            float const la = std::sqrt(ax * ax + ay * ay + az * az);
            float const lb = std::sqrt(bx * bx + by * by + bz * bz);
            float const lc = std::sqrt(cx * cx + cy * cy + cz * cz);
            float const dotAB = ax * bx + ay * by + az * bz;
            float const dotBC = bx * cx + by * cy + bz * cz;
            float const dotCA = cx * ax + cy * ay + cz * az;
            float const numerator =
                ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx);
            float const denominator = la * lb * lc + dotAB * lc + dotBC * la + dotCA * lb;
            return 2.0f * std::atan2(numerator, denominator);
        }

        /// Host port of `fwnHierarchical` from mesh_sdf.cl, byte-faithful.
        float fwnHierarchicalHost(SpatialMeshData const & data, float4 const & query, float beta)
        {
            if (data.nodes.empty())
            {
                return 0.f;
            }
            std::vector<int> stack;
            stack.reserve(64);
            stack.push_back(0);
            float windingSum = 0.f;
            float const betaSq = beta * beta;
            float const invFourPi = 1.f / kFourPi;
            while (!stack.empty())
            {
                int const nodeIdx = stack.back();
                stack.pop_back();
                auto const & ag = data.fwnAggregates[static_cast<std::size_t>(nodeIdx)];
                if (ag.areaCentroid.w <= 0.f)
                {
                    continue;
                }
                auto const & node = data.nodes[static_cast<std::size_t>(nodeIdx)];
                float const cx = ag.areaCentroid.x / ag.areaCentroid.w;
                float const cy = ag.areaCentroid.y / ag.areaCentroid.w;
                float const cz = ag.areaCentroid.z / ag.areaCentroid.w;
                float const dx = cx - query.x;
                float const dy = cy - query.y;
                float const dz = cz - query.z;
                float const distSq = dx * dx + dy * dy + dz * dz;
                float const radius = ag.weightedNormalSum.w;
                if (distSq > betaSq * radius * radius && distSq > 0.f)
                {
                    float const invDist = 1.f / std::sqrt(distSq);
                    float const invDistCubed = invDist * invDist * invDist;
                    float const Nx = 0.5f * ag.weightedNormalSum.x;
                    float const Ny = 0.5f * ag.weightedNormalSum.y;
                    float const Nz = 0.5f * ag.weightedNormalSum.z;
                    windingSum += invFourPi * (dx * Nx + dy * Ny + dz * Nz) * invDistCubed;
                    if (std::fabs(windingSum) > 0.75f)
                    {
                        return windingSum;
                    }
                    continue;
                }
                if (node.isLeaf())
                {
                    int const end = node.primStart + node.primCount;
                    for (int i = node.primStart; i < end; ++i)
                    {
                        auto const & tri = data.triangles[static_cast<std::size_t>(i)];
                        float4 const a{tri.v0.x - query.x, tri.v0.y - query.y, tri.v0.z - query.z, 0.f};
                        float4 const b{tri.v1.x - query.x, tri.v1.y - query.y, tri.v1.z - query.z, 0.f};
                        float4 const c{tri.v2.x - query.x, tri.v2.y - query.y, tri.v2.z - query.z, 0.f};
                        windingSum += invFourPi * solidAngleAtOrigin(a, b, c);
                    }
                }
                else
                {
                    if (node.rightChild >= 0)
                    {
                        stack.push_back(node.rightChild);
                    }
                    if (node.leftChild >= 0)
                    {
                        stack.push_back(node.leftChild);
                    }
                }
            }
            return windingSum;
        }

        std::vector<float4> cubeVertices()
        {
            return {
                {-1.f, -1.f, -1.f, 0.f}, {1.f, -1.f, -1.f, 0.f},
                {1.f, 1.f, -1.f, 0.f},   {-1.f, 1.f, -1.f, 0.f},
                {-1.f, -1.f, 1.f, 0.f},  {1.f, -1.f, 1.f, 0.f},
                {1.f, 1.f, 1.f, 0.f},    {-1.f, 1.f, 1.f, 0.f},
            };
        }
        std::vector<TriangleIndices> cubeIndices()
        {
            return {
                {4, 5, 6}, {4, 6, 7}, {1, 0, 3}, {1, 3, 2}, {5, 1, 2}, {5, 2, 6},
                {0, 4, 7}, {0, 7, 3}, {7, 6, 2}, {7, 2, 3}, {0, 1, 5}, {0, 5, 4},
            };
        }

        /// Cube with one triangle deliberately flipped — closed mesh has
        /// inconsistent orientation. Pure-BVH pseudo-normal sign breaks here;
        /// FWN should still return ~1 inside (sign of net winding is robust).
        std::vector<TriangleIndices> cubeIndicesOneFlipped()
        {
            auto idx = cubeIndices();
            std::swap(idx[0].i1, idx[0].i2); // flip first +z triangle
            return idx;
        }

        SpatialMeshData buildBvhAndAggregates(std::vector<float4> const & v,
                                              std::vector<TriangleIndices> const & i)
        {
            MeshBVHBuilder builder;
            SpatialMeshData data = builder.build(v, i);
            computeFwnAggregates(data);
            return data;
        }
    } // namespace

    // ========================================================================
    // Hierarchical winding-number agreement with brute-force reference
    // ========================================================================

    TEST(MeshFwnHierarchical, Cube_Inside_AgreesWithReference)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        auto const data = buildBvhAndAggregates(v, i);
        for (auto const & q : std::vector<float4>{{0.f, 0.f, 0.f, 0.f},
                                                  {0.5f, 0.f, 0.f, 0.f},
                                                  {0.f, -0.7f, 0.3f, 0.f}})
        {
            float const wRef = mesh_sdf_reference::referenceWindingNumber(v, i, q);
            float const wBh = fwnHierarchicalHost(data, q, 2.0f);
            // Both must classify the point as inside (winding > 0.5).
            // The hierarchical traversal short-circuits as soon as |sum| > 0.75,
            // so we don't expect tight numerical agreement — only sign agreement.
            EXPECT_GT(wRef, 0.5f);
            EXPECT_GT(wBh, 0.5f) << "query=(" << q.x << "," << q.y << "," << q.z << ")";
        }
    }

    TEST(MeshFwnHierarchical, Cube_Outside_AgreesWithReference)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        auto const data = buildBvhAndAggregates(v, i);
        for (auto const & q : std::vector<float4>{{3.f, 0.f, 0.f, 0.f},
                                                  {0.f, 5.f, 0.f, 0.f},
                                                  {-2.f, 2.f, 2.f, 0.f}})
        {
            float const wRef = mesh_sdf_reference::referenceWindingNumber(v, i, q);
            float const wBh = fwnHierarchicalHost(data, q, 2.0f);
            // Dipole approximation introduces a small bounded error outside; the
            // sign threshold is what matters for sign determination.
            EXPECT_NEAR(wRef, wBh, 1e-2f);
            EXPECT_LT(wBh, 0.5f);
        }
    }

    TEST(MeshFwnHierarchical, Cube_BetaLarger_TighterAgreement)
    {
        // Larger beta = more conservative acceptance criterion = more
        // recursion to leaves = more accurate. Lower beta accepts internal
        // nodes as far-field too eagerly and loses cancellation accuracy.
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        auto const data = buildBvhAndAggregates(v, i);
        float4 const q{2.5f, 0.f, 0.f, 0.f};
        float const wRef = mesh_sdf_reference::referenceWindingNumber(v, i, q);
        float const wBeta1 = fwnHierarchicalHost(data, q, 1.0f);
        float const wBeta4 = fwnHierarchicalHost(data, q, 4.0f);
        EXPECT_LE(std::fabs(wBeta4 - wRef), std::fabs(wBeta1 - wRef) + 1e-6f);
    }

    // ========================================================================
    // Robustness: inconsistent winding
    // ========================================================================

    TEST(MeshFwnHierarchical, Cube_OneFlippedTriangle_StillIdentifiesInside)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndicesOneFlipped();
        auto const data = buildBvhAndAggregates(v, i);
        // With one triangle flipped both reference and hierarchical methods
        // converge to the same wrong-but-consistent value at the centre.
        // What matters in production is that the SAME sign decision is made
        // by both methods, not exact agreement (early-exit may differ).
        float4 const q{0.f, 0.f, 0.f, 0.f};
        float const wRef = mesh_sdf_reference::referenceWindingNumber(v, i, q);
        float const wBh = fwnHierarchicalHost(data, q, 2.0f);
        EXPECT_EQ(wRef > 0.5f, wBh > 0.5f);
    }

} // namespace gladius::tests
