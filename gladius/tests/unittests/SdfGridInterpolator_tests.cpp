#include "compute/ManifoldSdfSampler.h"
#include "compute/SdfGridInterpolator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <type_traits>

namespace gladius::compute::tests
{
    static_assert(std::is_abstract_v<ManifoldSdfSampler>);
    TEST(SdfGridInterpolator, TrilinearSample_LinearField_ReturnsInterpolatedValue)
    {
        std::array<float, 8U> const grid{0.0F, 1.0F, 2.0F, 3.0F,
                                         4.0F, 5.0F, 6.0F, 7.0F};
        auto const sample = sampleTrilinearSdfGrid(
            Eigen::Vector3f::Zero(), Eigen::Vector3f(-1.0F, -2.0F, -3.0F),
            Eigen::Vector3f(2.0F, 4.0F, 6.0F), 2U, 2U, 2U,
            [&grid](std::size_t index) { return grid[index]; });

        EXPECT_FLOAT_EQ(sample, 3.5F);
    }

    TEST(SdfGridInterpolator, SampleOutsideBounds_ClampsToBoundaryValues)
    {
        std::array<float, 8U> const grid{0.0F, 1.0F, 2.0F, 3.0F,
                                         4.0F, 5.0F, 6.0F, 7.0F};
        auto const sampleMinimum = sampleTrilinearSdfGrid(
            Eigen::Vector3f(-100.0F, -100.0F, -100.0F), Eigen::Vector3f::Zero(),
            Eigen::Vector3f::Ones(), 2U, 2U, 2U,
            [&grid](std::size_t index) { return grid[index]; });
        auto const sampleMaximum = sampleTrilinearSdfGrid(
            Eigen::Vector3f(100.0F, 100.0F, 100.0F), Eigen::Vector3f::Zero(),
            Eigen::Vector3f::Ones(), 2U, 2U, 2U,
            [&grid](std::size_t index) { return grid[index]; });

        EXPECT_FLOAT_EQ(sampleMinimum, 0.0F);
        EXPECT_FLOAT_EQ(sampleMaximum, 7.0F);
    }

    TEST(SdfGridInterpolator, EmptyGrid_ReturnsZeroWithoutSampling)
    {
        bool wasSampled = false;
        auto const sample = sampleTrilinearSdfGrid(
            Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(),
            0U, 2U, 2U, [&wasSampled](std::size_t) {
                wasSampled = true;
                return 1.0F;
            });

        EXPECT_FLOAT_EQ(sample, 0.0F);
        EXPECT_FALSE(wasSampled);
    }
} // namespace gladius::compute::tests
