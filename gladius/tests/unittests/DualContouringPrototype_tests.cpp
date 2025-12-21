#include "DualContouringPrototype.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace gladius::dual_contouring;

namespace
{
    void expectBoundingBox(AxisAlignedBoundingBox const & bounds,
                           Eigen::Vector3f const & expectedMin,
                           Eigen::Vector3f const & expectedMax,
                           float tolerance)
    {
        ASSERT_LE(bounds.min.x(), bounds.max.x());
        ASSERT_LE(bounds.min.y(), bounds.max.y());
        ASSERT_LE(bounds.min.z(), bounds.max.z());

        EXPECT_NEAR(bounds.min.x(), expectedMin.x(), tolerance);
        EXPECT_NEAR(bounds.min.y(), expectedMin.y(), tolerance);
        EXPECT_NEAR(bounds.min.z(), expectedMin.z(), tolerance);

        EXPECT_NEAR(bounds.max.x(), expectedMax.x(), tolerance);
        EXPECT_NEAR(bounds.max.y(), expectedMax.y(), tolerance);
        EXPECT_NEAR(bounds.max.z(), expectedMax.z(), tolerance);
    }

    void expectOutwardOrientation(PrototypeMesh const & mesh)
    {
        ASSERT_EQ(mesh.faces.size(), mesh.faceNormals.size());
        for (size_t i = 0U; i < mesh.faces.size(); ++i)
        {
            Eigen::Vector3i const face = mesh.faces[i];
            Eigen::Vector3f const centroid = (mesh.vertices[static_cast<size_t>(face.x())] +
                                              mesh.vertices[static_cast<size_t>(face.y())] +
                                              mesh.vertices[static_cast<size_t>(face.z())]) /
                                             3.0F;
            float const centroidNorm = centroid.norm();
            if (centroidNorm < 1e-4F)
            {
                continue;
            }

            Eigen::Vector3f const expectedNormal = centroid / centroidNorm;
            float const dot = mesh.faceNormals[i].dot(expectedNormal);
            EXPECT_GT(dot, 0.0F) << "Face normal is not outward-facing";
        }
    }
}

TEST(DualContouringPrototypeTest, UnitCubeMeshMatchesAnalyticBounds)
{
    PrototypeDiagnostics diagnostics{};
    PrototypeMesh mesh = generatePrototypeMesh(PrototypeShape::UnitCube, 33U, &diagnostics);

    EXPECT_GT(diagnostics.vertexCount, 0U);
    EXPECT_GT(diagnostics.faceCount, 0U);
    EXPECT_EQ(diagnostics.invertedFaceCount, 0U);

    AxisAlignedBoundingBox const bounds = mesh.bounds();
    expectBoundingBox(bounds,
                      Eigen::Vector3f{-0.5F, -0.5F, -0.5F},
                      Eigen::Vector3f{0.5F, 0.5F, 0.5F},
                      0.05F);

    expectOutwardOrientation(mesh);

    EXPECT_GT(diagnostics.vertexCount, 500U);
    EXPECT_LT(diagnostics.vertexCount, 40000U);
}

TEST(DualContouringPrototypeTest, SphereMeshWithinExpectedRadius)
{
    PrototypeDiagnostics diagnostics{};
    PrototypeMesh mesh = generatePrototypeMesh(PrototypeShape::Sphere, 33U, &diagnostics);

    EXPECT_GT(diagnostics.vertexCount, 0U);
    EXPECT_GT(diagnostics.faceCount, 0U);
    EXPECT_EQ(diagnostics.invertedFaceCount, 0U);

    AxisAlignedBoundingBox const bounds = mesh.bounds();
    expectBoundingBox(bounds,
                      Eigen::Vector3f{-0.6F, -0.6F, -0.6F},
                      Eigen::Vector3f{0.6F, 0.6F, 0.6F},
                      0.05F);

    expectOutwardOrientation(mesh);

    EXPECT_GT(diagnostics.vertexCount, 800U);
    EXPECT_LT(diagnostics.vertexCount, 45000U);
}

TEST(DualContouringPrototypeTest, CylinderBlendMeshCoversCompositeShape)
{
    PrototypeDiagnostics diagnostics{};
    PrototypeMesh mesh = generatePrototypeMesh(PrototypeShape::CylinderBlend, 33U, &diagnostics);

    EXPECT_GT(diagnostics.vertexCount, 0U);
    EXPECT_GT(diagnostics.faceCount, 0U);
    EXPECT_EQ(diagnostics.invertedFaceCount, 0U);

    AxisAlignedBoundingBox const bounds = mesh.bounds();
    expectBoundingBox(bounds,
                      Eigen::Vector3f{-0.7F, -0.7F, -0.7F},
                      Eigen::Vector3f{0.7F, 0.7F, 0.7F},
                      0.08F);

    expectOutwardOrientation(mesh);

    EXPECT_GT(diagnostics.vertexCount, 800U);
    EXPECT_LT(diagnostics.vertexCount, 50000U);
}
