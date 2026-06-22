#include <gtest/gtest.h>

#include "io/3mf/ShellThicknessPartition.h"

namespace gladius::io::tests
{
    TEST(ShellThicknessPartition_Test, BuildIntervals_OrdersShellsFromOuterToInner)
    {
        ThicknessSolution solution(3);
        solution.thicknesses[0] = 1.5F; // bottom
        solution.thicknesses[1] = 0.5F;
        solution.thicknesses[2] = 0.25F; // top

        auto const intervals = ShellThicknessPartition::buildIntervals(solution);

        ASSERT_EQ(intervals.size(), 3U);

        EXPECT_EQ(intervals[0].layerIndex, 2U);
        EXPECT_FLOAT_EQ(intervals[0].outerDepth, 0.0F);
        EXPECT_FLOAT_EQ(intervals[0].innerDepth, 0.25F);

        EXPECT_EQ(intervals[1].layerIndex, 1U);
        EXPECT_FLOAT_EQ(intervals[1].outerDepth, 0.25F);
        EXPECT_FLOAT_EQ(intervals[1].innerDepth, 0.75F);

        EXPECT_EQ(intervals[2].layerIndex, 0U);
        EXPECT_FLOAT_EQ(intervals[2].outerDepth, 0.75F);
        EXPECT_FLOAT_EQ(intervals[2].innerDepth, 2.25F);
    }

    TEST(ShellThicknessPartition_Test, BuildIntervals_SkipsZeroThicknessLayers)
    {
        std::vector<float> const thicknesses{1.0F, 0.0F, 0.5F, 0.0F};

        auto const intervals = ShellThicknessPartition::buildIntervals(thicknesses);

        ASSERT_EQ(intervals.size(), 2U);
        EXPECT_EQ(intervals[0].layerIndex, 2U);
        EXPECT_EQ(intervals[1].layerIndex, 0U);
        EXPECT_FLOAT_EQ(intervals[1].outerDepth, 0.5F);
        EXPECT_FLOAT_EQ(intervals[1].innerDepth, 1.5F);
    }

    TEST(ShellThicknessPartition_Test, ComputeMaxDepth_ReturnsSumOfPositiveThicknesses)
    {
        std::vector<float> const thicknesses{0.4F, 0.0F, 0.6F, 1.2F};

        float const maxDepth = ShellThicknessPartition::computeMaxDepth(thicknesses);

        EXPECT_FLOAT_EQ(maxDepth, 2.2F);
    }

    TEST(ShellThicknessPartition_Test, BuildIntervals_WithNegativeThickness_Throws)
    {
        std::vector<float> const thicknesses{0.4F, -0.1F, 0.3F};

        EXPECT_THROW(static_cast<void>(ShellThicknessPartition::buildIntervals(thicknesses)),
                     std::runtime_error);
        EXPECT_THROW(static_cast<void>(ShellThicknessPartition::computeMaxDepth(thicknesses)),
                     std::runtime_error);
    }

    TEST(ShellThicknessPartition_Test, IntervalThickness_AndEmptyState_AreConsistent)
    {
        ShellLayerDepthInterval interval;
        interval.layerIndex = 1U;
        interval.outerDepth = 1.0F;
        interval.innerDepth = 1.0F + 1e-7F;

        EXPECT_NEAR(interval.thickness(), 1e-7F, 3e-8F);
        EXPECT_TRUE(interval.isEmpty());
        EXPECT_FALSE(interval.isEmpty(1e-8F));
    }
} // namespace gladius::io::tests