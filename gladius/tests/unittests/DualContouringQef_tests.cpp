#include "DualContouringQef.h"
#include "DualContouringOctree.h"

#include <gtest/gtest.h>

#include <cmath>

namespace gladius_tests
{
    using namespace gladius::dual_contouring;

    namespace
    {
        [[nodiscard]] AxisAlignedBoundingBox makeUnitBounds()
        {
            AxisAlignedBoundingBox bounds{};
            bounds.min = Eigen::Vector3f::Zero();
            bounds.max = Eigen::Vector3f::Ones();
            return bounds;
        }
    }

    TEST(QuadraticErrorFunction_Test, SolvesAxisAlignedPlanesIntersection)
    {
        QuadraticErrorFunction qef{};
        qef.addSample(Eigen::Vector3f{0.0F, 0.25F, 0.25F}, Eigen::Vector3f::UnitX());
        qef.addSample(Eigen::Vector3f{0.25F, 0.0F, 0.25F}, Eigen::Vector3f::UnitY());
        qef.addSample(Eigen::Vector3f{0.25F, 0.25F, 0.0F}, Eigen::Vector3f::UnitZ());

        Eigen::Vector3f position{};
        float residual = 0.0F;
        ASSERT_TRUE(qef.solve(position, residual));

        EXPECT_NEAR(position.x(), 0.0F, 1e-4F);
        EXPECT_NEAR(position.y(), 0.0F, 1e-4F);
        EXPECT_NEAR(position.z(), 0.0F, 1e-4F);
        EXPECT_NEAR(residual, 0.0F, 1e-5F);
    }

    TEST(QuadraticErrorFunction_Test, ClampsSolutionWithinBounds)
    {
        QuadraticErrorFunction qef{};
        qef.addSample(Eigen::Vector3f{2.0F, 0.0F, 0.0F}, Eigen::Vector3f::UnitX());
        qef.addSample(Eigen::Vector3f{0.0F, 2.0F, 0.0F}, Eigen::Vector3f::UnitY());
        qef.addSample(Eigen::Vector3f{0.0F, 0.0F, 2.0F}, Eigen::Vector3f::UnitZ());

        AxisAlignedBoundingBox bounds = makeUnitBounds();

        Eigen::Vector3f position{};
        float residual = 0.0F;
        ASSERT_TRUE(qef.solveWithinBounds(bounds, position, residual));

        EXPECT_GE(position.x(), bounds.min.x());
        EXPECT_LE(position.x(), bounds.max.x());
        EXPECT_GE(position.y(), bounds.min.y());
        EXPECT_LE(position.y(), bounds.max.y());
        EXPECT_GE(position.z(), bounds.min.z());
        EXPECT_LE(position.z(), bounds.max.z());
    }

    TEST(QuadraticErrorFunction_Test, RejectsInsufficientSamples)
    {
        QuadraticErrorFunction qef{};
        qef.addSample(Eigen::Vector3f{0.0F, 0.25F, 0.25F}, Eigen::Vector3f::UnitX());

        Eigen::Vector3f position{};
        float residual = 0.0F;
        EXPECT_FALSE(qef.solve(position, residual));
        EXPECT_FALSE(position.allFinite());
        EXPECT_FALSE(std::isfinite(residual));
    }
}
