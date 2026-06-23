#include <gtest/gtest.h>

#include "io/3mf/OpenVdbShellGenerator.h"

namespace gladius::io::tests
{
    TEST(OpenVdbShellGenerator_Test, EvaluateShellSignedDistance_IsPositiveOutsideModel)
    {
        ShellLayerDepthInterval interval;
        interval.layerIndex = 0U;
        interval.outerDepth = 0.2F;
        interval.innerDepth = 0.6F;

        EXPECT_GT(OpenVdbShellGenerator::evaluateShellSignedDistance(0.1F, interval), 0.0F);
        EXPECT_FLOAT_EQ(OpenVdbShellGenerator::evaluateShellSignedDistance(-0.2F, interval), 0.0F);
    }

    TEST(OpenVdbShellGenerator_Test, EvaluateShellSignedDistance_IsNegativeInsideShellBand)
    {
        ShellLayerDepthInterval interval;
        interval.layerIndex = 1U;
        interval.outerDepth = 0.2F;
        interval.innerDepth = 0.6F;

        EXPECT_LT(OpenVdbShellGenerator::evaluateShellSignedDistance(-0.35F, interval), 0.0F);
        EXPECT_LT(OpenVdbShellGenerator::evaluateShellSignedDistance(-0.6F + 1e-4F, interval), 0.0F);
    }

    TEST(OpenVdbShellGenerator_Test, EvaluateShellSignedDistance_IsPositiveDeeperThanInnerBoundary)
    {
        ShellLayerDepthInterval interval;
        interval.layerIndex = 2U;
        interval.outerDepth = 0.2F;
        interval.innerDepth = 0.6F;

        EXPECT_FLOAT_EQ(OpenVdbShellGenerator::evaluateShellSignedDistance(-0.6F, interval), 0.0F);
        EXPECT_GT(OpenVdbShellGenerator::evaluateShellSignedDistance(-0.8F, interval), 0.0F);
    }

    TEST(OpenVdbShellGenerator_Test, EvaluateVariableShellSignedDistance_IsNegativeInsideVariableBand)
    {
        EXPECT_LT(OpenVdbShellGenerator::evaluateVariableShellSignedDistance(
                      -0.35F,
                      0.2F,
                      0.6F,
                      false),
                  0.0F);
    }

    TEST(OpenVdbShellGenerator_Test, EvaluateVariableShellSignedDistance_InnermostUsesOnlyOuterBoundary)
    {
        EXPECT_FLOAT_EQ(OpenVdbShellGenerator::evaluateVariableShellSignedDistance(
                            -0.2F,
                            0.2F,
                            0.7F,
                            true),
                        0.0F);
        EXPECT_LT(OpenVdbShellGenerator::evaluateVariableShellSignedDistance(
                      -0.5F,
                      0.2F,
                      0.7F,
                      true),
                  0.0F);
    }
} // namespace gladius::io::tests