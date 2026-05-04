/**
 * @file MeshSamplingGeometry_tests.cpp
 * @brief Tests for method-agnostic mesh color sampling geometry.
 */

#include "io/3mf/MeshSamplingGeometry.h"

#include "ComputeContext.h"
#include "Mesh.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gladius_tests
{
    using namespace gladius;
    using namespace gladius::io;

    TEST(MeshSamplingGeometryTest, FromTriangleSoupMesh_PreservesFaceOrder)
    {
        ComputeContext context;
        Mesh mesh(context);

        Vector3 const a{0.0F, 0.0F, 0.0F};
        Vector3 const b{1.0F, 0.0F, 0.0F};
        Vector3 const c{0.0F, 1.0F, 0.0F};
        Vector3 const d{2.0F, 0.0F, 0.0F};
        Vector3 const e{2.0F, 1.0F, 0.0F};
        Vector3 const f{2.0F, 0.0F, 1.0F};

        mesh.addFace(a, b, c);
        mesh.addFace(d, e, f);

        auto const geometry = MeshSamplingGeometry::fromTriangleSoupMesh(mesh);

        ASSERT_EQ(geometry.vertices.size(), 6U);
        ASSERT_EQ(geometry.faces.size(), 2U);
        EXPECT_TRUE(geometry.matchesFaceCount(mesh));

        EXPECT_EQ(geometry.faces[0], (std::array<std::uint32_t, 3>{0U, 1U, 2U}));
        EXPECT_EQ(geometry.faces[1], (std::array<std::uint32_t, 3>{3U, 4U, 5U}));

        EXPECT_FLOAT_EQ(geometry.vertices[0].x(), a.x());
        EXPECT_FLOAT_EQ(geometry.vertices[0].y(), a.y());
        EXPECT_FLOAT_EQ(geometry.vertices[0].z(), a.z());
        EXPECT_FLOAT_EQ(geometry.vertices[4].x(), e.x());
        EXPECT_FLOAT_EQ(geometry.vertices[4].y(), e.y());
        EXPECT_FLOAT_EQ(geometry.vertices[4].z(), e.z());
    }

    TEST(MeshSamplingGeometryTest, FromIndexedTriangles_PreservesSharedVerticesAndFaceOrder)
    {
        std::vector<Eigen::Vector3f> const positions{{0.0F, 0.0F, 0.0F},
                                                     {1.0F, 0.0F, 0.0F},
                                                     {0.0F, 1.0F, 0.0F},
                                                     {0.0F, 0.0F, 1.0F}};
        std::vector<std::uint32_t> const indices{0U, 1U, 2U, 2U, 1U, 3U};

        auto const geometry = MeshSamplingGeometry::fromIndexedTriangles(positions, indices);

        EXPECT_EQ(geometry.vertices, positions);
        ASSERT_EQ(geometry.faces.size(), 2U);
        EXPECT_EQ(geometry.faces[0], (std::array<std::uint32_t, 3>{0U, 1U, 2U}));
        EXPECT_EQ(geometry.faces[1], (std::array<std::uint32_t, 3>{2U, 1U, 3U}));
    }

    TEST(MeshSamplingGeometryTest, FromIndexedTriangles_WithIncompleteTriangle_Throws)
    {
        std::vector<Eigen::Vector3f> const positions{{0.0F, 0.0F, 0.0F},
                                                     {1.0F, 0.0F, 0.0F},
                                                     {0.0F, 1.0F, 0.0F}};
        std::vector<std::uint32_t> const indices{0U, 1U};

        EXPECT_THROW(MeshSamplingGeometry::fromIndexedTriangles(positions, indices),
                     std::runtime_error);
    }

    TEST(MeshSamplingGeometryTest, FromIndexedTriangles_WithOutOfRangeIndex_Throws)
    {
        std::vector<Eigen::Vector3f> const positions{{0.0F, 0.0F, 0.0F},
                                                     {1.0F, 0.0F, 0.0F},
                                                     {0.0F, 1.0F, 0.0F}};
        std::vector<std::uint32_t> const indices{0U, 1U, 3U};

        EXPECT_THROW(MeshSamplingGeometry::fromIndexedTriangles(positions, indices),
                     std::runtime_error);
    }
} // namespace gladius_tests
