/// @file MeshSdfReference_tests.cpp
/// @brief Unit tests for the brute-force host reference SDF.
/// @see MeshSdfReference.h

#include "MeshSdfReference.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        std::vector<float4> cubeVertices()
        {
            return {
                {-1.f, -1.f, -1.f, 0.f},
                { 1.f, -1.f, -1.f, 0.f},
                { 1.f,  1.f, -1.f, 0.f},
                {-1.f,  1.f, -1.f, 0.f},
                {-1.f, -1.f,  1.f, 0.f},
                { 1.f, -1.f,  1.f, 0.f},
                { 1.f,  1.f,  1.f, 0.f},
                {-1.f,  1.f,  1.f, 0.f},
            };
        }
        std::vector<TriangleIndices> cubeIndices()
        {
            // Outward-facing triangles for a unit cube of half-extent 1.
            return {
                {4, 5, 6}, {4, 6, 7}, // +z
                {1, 0, 3}, {1, 3, 2}, // -z
                {5, 1, 2}, {5, 2, 6}, // +x
                {0, 4, 7}, {0, 7, 3}, // -x
                {7, 6, 2}, {7, 2, 3}, // +y
                {0, 1, 5}, {0, 5, 4}, // -y
            };
        }

        /// Subdivided icosahedron approximation of a unit sphere.
        /// Adequate for sphere-distance verification (max error ~ 1/N).
        void buildIcosphere(std::vector<float4> & verts,
                            std::vector<TriangleIndices> & tris,
                            int subdivisions)
        {
            verts.clear();
            tris.clear();
            constexpr float t = 1.6180339887498949f; // golden ratio
            std::vector<float4> v0 = {
                {-1.f,  t, 0.f, 0.f}, { 1.f,  t, 0.f, 0.f},
                {-1.f, -t, 0.f, 0.f}, { 1.f, -t, 0.f, 0.f},
                {0.f, -1.f,  t, 0.f}, {0.f,  1.f,  t, 0.f},
                {0.f, -1.f, -t, 0.f}, {0.f,  1.f, -t, 0.f},
                { t, 0.f, -1.f, 0.f}, { t, 0.f,  1.f, 0.f},
                {-t, 0.f, -1.f, 0.f}, {-t, 0.f,  1.f, 0.f},
            };
            std::vector<TriangleIndices> t0 = {
                {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
                {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
                {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
            };
            auto normalise = [](float4 const & p) -> float4
            {
                float const len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                return {p.x / len, p.y / len, p.z / len, 0.f};
            };
            for (auto & p : v0) { p = normalise(p); }
            for (int s = 0; s < subdivisions; ++s)
            {
                std::vector<TriangleIndices> tNext;
                tNext.reserve(t0.size() * 4u);
                for (auto const & tri : t0)
                {
                    float4 const & a = v0[static_cast<std::size_t>(tri.i0)];
                    float4 const & b = v0[static_cast<std::size_t>(tri.i1)];
                    float4 const & c = v0[static_cast<std::size_t>(tri.i2)];
                    int const ab = static_cast<int>(v0.size());
                    v0.push_back(normalise({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
                                            (a.z + b.z) * 0.5f, 0.f}));
                    int const bc = static_cast<int>(v0.size());
                    v0.push_back(normalise({(b.x + c.x) * 0.5f, (b.y + c.y) * 0.5f,
                                            (b.z + c.z) * 0.5f, 0.f}));
                    int const ca = static_cast<int>(v0.size());
                    v0.push_back(normalise({(c.x + a.x) * 0.5f, (c.y + a.y) * 0.5f,
                                            (c.z + a.z) * 0.5f, 0.f}));
                    tNext.push_back({tri.i0, ab, ca});
                    tNext.push_back({tri.i1, bc, ab});
                    tNext.push_back({tri.i2, ca, bc});
                    tNext.push_back({ab, bc, ca});
                }
                t0 = std::move(tNext);
            }
            verts = std::move(v0);
            tris = std::move(t0);
        }
    } // namespace

    using mesh_sdf_reference::referenceSignedDistance;
    using mesh_sdf_reference::referenceUnsignedDistance;
    using mesh_sdf_reference::referenceWindingNumber;

    // ========================================================================
    // Unsigned distance
    // ========================================================================

    TEST(MeshSdfReference_UnsignedDistance, EmptyMesh_ReturnsZero)
    {
        std::vector<float4> v;
        std::vector<TriangleIndices> i;
        EXPECT_FLOAT_EQ(referenceUnsignedDistance(v, i, {1.f, 2.f, 3.f, 0.f}), 0.f);
    }

    TEST(MeshSdfReference_UnsignedDistance, Cube_OutsideAlongAxis_MatchesAnalytic)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        // Point at (3, 0, 0): cube has half-extent 1 → distance 2.
        float const d = referenceUnsignedDistance(v, i, {3.f, 0.f, 0.f, 0.f});
        EXPECT_NEAR(d, 2.f, 1e-5f);
    }

    TEST(MeshSdfReference_UnsignedDistance, Cube_InsidePoint_HasSmallDistance)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        // Origin is centre of cube → closest face at distance 1.
        float const d = referenceUnsignedDistance(v, i, {0.f, 0.f, 0.f, 0.f});
        EXPECT_NEAR(d, 1.f, 1e-5f);
    }

    TEST(MeshSdfReference_UnsignedDistance, Sphere_OnSurface_NearZero)
    {
        std::vector<float4> v;
        std::vector<TriangleIndices> i;
        buildIcosphere(v, i, 3); // ~1280 triangles
        float const d = referenceUnsignedDistance(v, i, {1.f, 0.f, 0.f, 0.f});
        EXPECT_LT(d, 5e-3f);
    }

    // ========================================================================
    // Winding number
    // ========================================================================

    TEST(MeshSdfReference_WindingNumber, ClosedCube_Inside_NearOne)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        EXPECT_NEAR(referenceWindingNumber(v, i, {0.f, 0.f, 0.f, 0.f}), 1.f, 1e-4f);
    }

    TEST(MeshSdfReference_WindingNumber, ClosedCube_Outside_NearZero)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        EXPECT_NEAR(referenceWindingNumber(v, i, {3.f, 0.f, 0.f, 0.f}), 0.f, 1e-4f);
    }

    TEST(MeshSdfReference_WindingNumber, OpenQuad_FarFromQuad_NearZero)
    {
        // Single triangle in the xy-plane (open mesh). Winding is small
        // everywhere except inside the projection of the triangle.
        std::vector<float4> v = {
            {0.f, 0.f, 0.f, 0.f},
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
        };
        std::vector<TriangleIndices> i = {{0, 1, 2}};
        // Far from the triangle: winding should be very close to zero.
        EXPECT_NEAR(referenceWindingNumber(v, i, {10.f, 10.f, 10.f, 0.f}), 0.f, 1e-3f);
    }

    TEST(MeshSdfReference_WindingNumber, Sphere_Centre_NearOne)
    {
        std::vector<float4> v;
        std::vector<TriangleIndices> i;
        buildIcosphere(v, i, 0); // raw icosahedron, watertight + CCW outward
        EXPECT_NEAR(referenceWindingNumber(v, i, {0.f, 0.f, 0.f, 0.f}), 1.f, 1e-4f);
    }

    // ========================================================================
    // Combined signed distance
    // ========================================================================

    TEST(MeshSdfReference_SignedDistance, Cube_InsideIsNegative)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        EXPECT_LT(referenceSignedDistance(v, i, {0.f, 0.f, 0.f, 0.f}), 0.f);
    }

    TEST(MeshSdfReference_SignedDistance, Cube_OutsideIsPositive)
    {
        auto const v = cubeVertices();
        auto const i = cubeIndices();
        EXPECT_GT(referenceSignedDistance(v, i, {2.f, 0.f, 0.f, 0.f}), 0.f);
    }

    TEST(MeshSdfReference_SignedDistance, Sphere_OutsideIsPositive)
    {
        std::vector<float4> v;
        std::vector<TriangleIndices> i;
        buildIcosphere(v, i, 0);
        // Outside the unit sphere → positive sign.
        EXPECT_GT(referenceSignedDistance(v, i, {2.f, 0.f, 0.f, 0.f}), 0.f);
        // Inside the icosahedron (which approximates the unit sphere) → negative.
        EXPECT_LT(referenceSignedDistance(v, i, {0.f, 0.f, 0.f, 0.f}), 0.f);
    }

} // namespace gladius::tests
