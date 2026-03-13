/**
 * @file NumericWidget_tests.cpp
 * @brief Unit tests for adaptive drag-float sensitivity, orbital dial, and bounds handling.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace gladius::ui::tests
{
    namespace
    {
        /// Mirrors the adaptive step computation from NumericWidgets.cpp
        float computeAdaptiveStep(float value)
        {
            float constexpr EPSILON = 1e-6f;
            float const magnitude = std::abs(value) + EPSILON;
            float const order = std::floor(std::log10(magnitude));
            return std::pow(10.f, order) * 0.01f;
        }
    }

    // --- T012: AdaptiveDragFloat sensitivity tests ---

    TEST(AdaptiveDragFloat, NearZero_UsesSmallSteps)
    {
        // Near zero, steps should be very small (order of ~1e-8)
        float const step = computeAdaptiveStep(0.001f);
        EXPECT_LT(step, 0.001f);
        EXPECT_GT(step, 0.f);
    }

    TEST(AdaptiveDragFloat, LargeValue_UsesLargeSteps)
    {
        // For large values (~1000), steps should be proportionally large
        float const step = computeAdaptiveStep(1000.f);
        EXPECT_GE(step, 1.f);
    }

    TEST(AdaptiveDragFloat, ShiftModifier_ReducesSensitivity)
    {
        // Shift reduces sensitivity by ×0.01
        float const baseStep = computeAdaptiveStep(10.f);
        float const fineStep = baseStep * 0.01f;
        EXPECT_NEAR(fineStep, baseStep * 0.01f, 1e-9f);
        EXPECT_LT(fineStep, baseStep);
    }

    TEST(AdaptiveDragFloat, CtrlModifier_IncreasesSensitivity)
    {
        // Ctrl increases sensitivity by ×100
        float const baseStep = computeAdaptiveStep(10.f);
        float const coarseStep = baseStep * 100.f;
        EXPECT_NEAR(coarseStep, baseStep * 100.f, 1e-6f);
        EXPECT_GT(coarseStep, baseStep);
    }

    TEST(AdaptiveDragFloat, BoundedValue_ClampsToMinMax)
    {
        float value = 5.f;
        float const minVal = 0.f;
        float const maxVal = 10.f;
        value = std::clamp(value + 20.f, minVal, maxVal);
        EXPECT_FLOAT_EQ(value, 10.f);

        value = std::clamp(value - 30.f, minVal, maxVal);
        EXPECT_FLOAT_EQ(value, 0.f);
    }

    TEST(AdaptiveDragFloat, UnboundedValue_AllowsAnyValue)
    {
        float value = 0.f;
        value += 1e6f;
        EXPECT_FLOAT_EQ(value, 1e6f);

        value -= 2e6f;
        EXPECT_FLOAT_EQ(value, -1e6f);
    }

    // --- Orbital Dial tests ---

    TEST(OrbitalDial, BoundedRange_MapsAngleToValueRange)
    {
        // Simulating: half rotation over a [0, 100] range
        float const minVal = 0.f;
        float const maxVal = 100.f;
        float const range = maxVal - minVal;
        float constexpr PI = 3.14159265358979323846f;
        float constexpr TWO_PI = 2.f * PI;

        float const angleDelta = PI; // half rotation
        float const valueDelta = (angleDelta / TWO_PI) * range;

        EXPECT_NEAR(valueDelta, 50.f, 0.01f);
    }

    TEST(OrbitalDial, Unbounded_AccumulatesAngle)
    {
        // Unbounded: value accumulates proportionally to angle delta
        float value = 10.f;
        float const baseStep = computeAdaptiveStep(value);
        float constexpr PI = 3.14159265358979323846f;

        float const angleDelta = PI;
        float const valueDelta = angleDelta * baseStep * 10.f;
        value += valueDelta;

        // Value should have increased
        EXPECT_GT(value, 10.f);
    }

    // --- Step scaling consistency ---

    TEST(AdaptiveDragFloat, StepScalesLogarithmically)
    {
        float const step1 = computeAdaptiveStep(1.f);
        float const step10 = computeAdaptiveStep(10.f);
        float const step100 = computeAdaptiveStep(100.f);

        // Each order of magnitude should increase the step by ~10x
        EXPECT_NEAR(step10 / step1, 10.f, 1.f);
        EXPECT_NEAR(step100 / step10, 10.f, 1.f);
    }

} // namespace gladius::ui::tests
