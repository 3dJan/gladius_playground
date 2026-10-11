#include <gtest/gtest.h>

#include "ui/NumericInputOverlay.h"

#include <cmath>
#include <limits>

namespace gladius::ui::tests
{
    TEST(NumericInputOverlay, ParseFloat_ValidDecimalAndExponent_ReturnsParsedValue)
    {
        auto const parsed = numeric_input_detail::parseFloat("-1.25e2");

        ASSERT_TRUE(parsed.has_value());
        EXPECT_FLOAT_EQ(*parsed, -125.f);
    }

    TEST(NumericInputOverlay, ParseFloat_IncompleteTrailingOrNonFiniteValue_ReturnsNoValue)
    {
        for (std::string_view text : {"", ".", "1.2tail", "1.2e", "nan", "inf"})
        {
            EXPECT_FALSE(numeric_input_detail::parseFloat(text).has_value()) << text;
        }
    }

    TEST(NumericInputOverlay, InitialStep_UsesDecimalPowersBasedOnValueMagnitude)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::initialStep(0.f), 0.01f);
        EXPECT_FLOAT_EQ(numeric_input_detail::initialStep(12.5f), 0.01f);
        EXPECT_FLOAT_EQ(numeric_input_detail::initialStep(0.5f), 0.0001f);
        EXPECT_FLOAT_EQ(numeric_input_detail::initialStep(0.003f), 0.000001f);
        EXPECT_FLOAT_EQ(numeric_input_detail::initialStep(-25.f), 0.01f);
    }

    TEST(NumericInputOverlay, FormatInputValue_AddsTwoDecimalsAndTrimsFloatNoise)
    {
        EXPECT_EQ(numeric_input_detail::formatInputValue(12.5f), "12.50");
        EXPECT_EQ(numeric_input_detail::formatInputValue(6.0190005f), "6.019");
        EXPECT_EQ(numeric_input_detail::formatInputValue(100.f), "100.00");
    }

    TEST(NumericInputOverlay, FormatStep_PowersOfTen_HidesFloatRepresentationNoise)
    {
        EXPECT_EQ(numeric_input_detail::formatStep(0.000999999931f), "0.001");
        EXPECT_EQ(numeric_input_detail::formatStep(0.00999999978f), "0.01");
        EXPECT_EQ(numeric_input_detail::formatStep(0.100000001f), "0.1");
        EXPECT_EQ(numeric_input_detail::formatStep(1.f), "1");
    }

    TEST(NumericInputOverlay, FormatRulerValue_PowerOfTenMajorStep_UsesExpectedDecimals)
    {
        EXPECT_EQ(numeric_input_detail::formatRulerValue(12.3f, 0.01f), "12.3");
    }

    TEST(NumericInputOverlay, RulerOffsetForValue_ValueBetweenMajorTicks_AlignsTicksToExactValues)
    {
        float constexpr value = 95.1854858f;
        float constexpr step = 0.1f;
        double const offset = numeric_input_detail::rulerOffsetForValue(value, step);
        double const majorStep = static_cast<double>(step) * 10.0;

        EXPECT_FLOAT_EQ(static_cast<float>(static_cast<double>(value) + offset * step), 95.f);
        EXPECT_FLOAT_EQ(static_cast<float>(static_cast<double>(value) + (offset + 10.0) * step),
                        96.f);
        EXPECT_NEAR(std::abs(offset), 1.85, 0.01);
        EXPECT_NEAR(majorStep, 1.0, 1e-6);
    }

    TEST(NumericInputOverlay, ScaleValueByDecade_ScalesPositiveAndNegativeValues)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::scaleValueByDecade(125.f, -1), 12.5f);
        EXPECT_FLOAT_EQ(numeric_input_detail::scaleValueByDecade(-12.5f, 1), -125.f);
    }

    TEST(NumericInputOverlay, ScaleValueByDecade_ZeroAndExtremeValues_RemainFinite)
    {
        float const limit = std::numeric_limits<float>::max();
        float const zero = numeric_input_detail::scaleValueByDecade(0.f, 1);
        float const capped = numeric_input_detail::scaleValueByDecade(limit, 1);

        EXPECT_FLOAT_EQ(zero, 0.f);
        EXPECT_TRUE(std::isfinite(capped));
        EXPECT_FLOAT_EQ(capped, limit);
    }

    TEST(NumericInputOverlay, ScaleStepByDecade_FinerAndCoarserChangeOnlyTheRulerStep)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::scaleStepByDecade(0.01f, -1), 0.001f);
        EXPECT_FLOAT_EQ(numeric_input_detail::scaleStepByDecade(0.01f, 1), 0.1f);
    }

    TEST(NumericInputOverlay, WheelStepCount_NormalizesPlatformMagnitudeToOneStep)
    {
        EXPECT_EQ(numeric_input_detail::wheelStepCount(120.f), 1);
        EXPECT_EQ(numeric_input_detail::wheelStepCount(-120.f), -1);
        EXPECT_EQ(numeric_input_detail::wheelStepCount(0.f), 0);
        EXPECT_EQ(numeric_input_detail::wheelStepCount(
                    std::numeric_limits<float>::infinity()),
                  0);
    }

    TEST(NumericInputOverlay, NudgeValue_PositiveAndNegativeDirection_AppliesStep)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(1.f, 0.25f, 1), 1.25f);
        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(1.f, 0.25f, -1), 0.75f);
    }

    TEST(NumericInputOverlay, NudgeValue_MultipleSteps_AppliesStepCount)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(1.f, 0.25f, 3), 1.75f);
        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(1.f, 0.25f, -3), 0.25f);
    }

    TEST(NumericInputOverlay, NudgeValue_AtFloatLimit_DoesNotOverflow)
    {
        float const limit = std::numeric_limits<float>::max();

        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(limit, limit, 1), limit);
        EXPECT_FLOAT_EQ(numeric_input_detail::nudgeValue(-limit, limit, -1), -limit);
    }

    TEST(NumericInputOverlay, ConstantValue_AngleMode_UsesDegreeValuesForPiAndTau)
    {
        EXPECT_FLOAT_EQ(numeric_input_detail::constantValue(NumericConstant::Pi, true), 180.f);
        EXPECT_FLOAT_EQ(numeric_input_detail::constantValue(NumericConstant::Tau, true), 360.f);
        EXPECT_NEAR(numeric_input_detail::constantValue(NumericConstant::Pi, false),
                    3.14159265f,
                    1e-6f);
    }

    TEST(NumericInputOverlay, ConstantValue_AdditionalConstants_AreFinite)
    {
        for (NumericConstant constant : {NumericConstant::Euler,
                                         NumericConstant::SqrtTwo,
                                         NumericConstant::GoldenRatio,
                                         NumericConstant::SqrtThree,
                                         NumericConstant::LnTwo,
                                         NumericConstant::LnTen})
        {
            EXPECT_TRUE(std::isfinite(numeric_input_detail::constantValue(constant, false)));
        }
    }

    TEST(NumericInputOverlay, AngleConversion_DegreesAndRadians_RoundTrip)
    {
        float const radians = numeric_input_detail::degreesToRadians(180.f);
        EXPECT_NEAR(radians, 3.14159265f, 1e-6f);
        EXPECT_NEAR(numeric_input_detail::radiansToDegrees(radians), 180.f, 1e-5f);
    }
} // namespace gladius::ui::tests
