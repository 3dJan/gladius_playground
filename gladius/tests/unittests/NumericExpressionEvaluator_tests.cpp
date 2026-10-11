#include <gtest/gtest.h>

#include "ui/NumericExpressionEvaluator.h"

#include <cmath>
#include <limits>

namespace gladius::ui::tests
{
    using numeric_input_detail::evaluateNumericExpression;
    using numeric_input_detail::formatEngineeringValue;

    TEST(NumericExpressionEvaluator, Evaluate_ArithmeticAndParentheses_UsesExpectedPrecedence)
    {
        auto const result = evaluateNumericExpression("2*(3+4)", false);

        ASSERT_TRUE(result.value.has_value()) << result.error;
        EXPECT_FLOAT_EQ(*result.value, 14.f);
    }

    TEST(NumericExpressionEvaluator, Evaluate_PowerAndCommonFunctions_ReturnsExpectedValue)
    {
        auto const power = evaluateNumericExpression("2^3 + pow(2, 2)", false);
        auto const functions = evaluateNumericExpression("sqrt(9) + ln(e) + log10(100)", false);

        ASSERT_TRUE(power.value.has_value()) << power.error;
        ASSERT_TRUE(functions.value.has_value()) << functions.error;
        EXPECT_FLOAT_EQ(*power.value, 12.f);
        EXPECT_NEAR(*functions.value, 6.f, 1e-6f);
    }

    TEST(NumericExpressionEvaluator, Evaluate_TrigFunctions_UsesRadiansForNonAngleValues)
    {
        auto const result = evaluateNumericExpression("sin(pi/2)", false);

        ASSERT_TRUE(result.value.has_value()) << result.error;
        EXPECT_NEAR(*result.value, 1.f, 1e-6f);
    }

    TEST(NumericExpressionEvaluator, Evaluate_AngleTarget_UsesDegreesForConstantsAndTrig)
    {
        auto const sine = evaluateNumericExpression("sin(pi/6)", true);
        auto const inverseSine = evaluateNumericExpression("asin(0.5)", true);
        auto const atan2 = evaluateNumericExpression("atan2(1,1)", true);
        auto const angle = evaluateNumericExpression("tau", true);

        ASSERT_TRUE(sine.value.has_value()) << sine.error;
        ASSERT_TRUE(inverseSine.value.has_value()) << inverseSine.error;
        ASSERT_TRUE(atan2.value.has_value()) << atan2.error;
        ASSERT_TRUE(angle.value.has_value()) << angle.error;
        EXPECT_NEAR(*sine.value, 0.5f, 1e-6f);
        EXPECT_NEAR(*inverseSine.value, 30.f, 1e-5f);
        EXPECT_NEAR(*atan2.value, 45.f, 1e-5f);
        EXPECT_FLOAT_EQ(*angle.value, 360.f);
    }

    TEST(NumericExpressionEvaluator, Evaluate_UnknownNamesAndMalformedExpressions_ReturnsError)
    {
        for (std::string_view expression : {"x + 1", "min(1, 2)", "2 +", "sin 30", "1 && 2"})
        {
            auto const result = evaluateNumericExpression(expression, false);
            EXPECT_FALSE(result.value.has_value()) << expression;
            EXPECT_FALSE(result.error.empty()) << expression;
        }
    }

    TEST(NumericExpressionEvaluator, Evaluate_DomainAndRangeErrors_RejectsResult)
    {
        for (std::string_view expression : {"1/0", "sqrt(-1)", "1e40", "1e-50"})
        {
            auto const result = evaluateNumericExpression(expression, false);
            EXPECT_FALSE(result.value.has_value()) << expression;
            EXPECT_FALSE(result.error.empty()) << expression;
        }
    }

    TEST(NumericExpressionEvaluator, FormatEngineeringValue_ExtremeMagnitudes_UsesEngineeringExponent)
    {
        EXPECT_EQ(formatEngineeringValue(1.23e6f), "1.23e+06");
        EXPECT_EQ(formatEngineeringValue(560e-6f), "560e-06");
        EXPECT_EQ(formatEngineeringValue(123.5f), "123.5");
        EXPECT_EQ(formatEngineeringValue(0.f), "0");
        EXPECT_EQ(formatEngineeringValue(std::numeric_limits<float>::max()), "340.282e+36");
    }
} // namespace gladius::ui::tests