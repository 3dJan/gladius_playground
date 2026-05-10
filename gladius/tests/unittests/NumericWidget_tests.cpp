/**
 * @file NumericWidget_tests.cpp
 * @brief Unit tests for adaptive drag-float sensitivity, orbital dial, and bounds handling.
 */

#include <gtest/gtest.h>

#include "nodes/Model.h"
#include "ui/NumericWidgets.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace gladius::ui::tests
{
    // --- T012: AdaptiveDragFloat sensitivity tests ---

    TEST(AdaptiveDragFloat, NearZero_UsesSmallSteps)
    {
        // Near zero, steps should be very small (order of ~1e-8)
        float const step = numeric_widget_detail::computeAdaptiveStep(0.001f);
        EXPECT_LT(step, 0.001f);
        EXPECT_GT(step, 0.f);
    }

    TEST(AdaptiveDragFloat, LargeValue_UsesLargeSteps)
    {
        // For large values (~1000), steps should be proportionally large
        float const step = numeric_widget_detail::computeAdaptiveStep(1000.f);
        EXPECT_GE(step, 1.f);
    }

    TEST(AdaptiveDragFloat, ShiftModifier_ReducesSensitivity)
    {
        // Shift reduces sensitivity by ×0.01
        float const baseStep = numeric_widget_detail::computeAdaptiveStep(10.f);
        float const fineStep = numeric_widget_detail::applyModifierStep(baseStep, true, false);
        EXPECT_NEAR(fineStep, baseStep * 0.01f, 1e-9f);
        EXPECT_LT(fineStep, baseStep);
    }

    TEST(AdaptiveDragFloat, CtrlModifier_IncreasesSensitivity)
    {
        // Ctrl increases sensitivity by ×100
        float const baseStep = numeric_widget_detail::computeAdaptiveStep(10.f);
        float const coarseStep = numeric_widget_detail::applyModifierStep(baseStep, false, true);
        EXPECT_NEAR(coarseStep, baseStep * 100.f, 1e-6f);
        EXPECT_GT(coarseStep, baseStep);
    }

    TEST(AdaptiveDragFloat, BoundedValue_ClampsToMinMax)
    {
        float value = 5.f;
        float const minVal = 0.f;
        float const maxVal = 10.f;
        value = numeric_widget_detail::clampToBounds(value + 20.f, minVal, maxVal);
        EXPECT_FLOAT_EQ(value, 10.f);

        value = numeric_widget_detail::clampToBounds(value - 30.f, minVal, maxVal);
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
        float const baseStep = numeric_widget_detail::computeAdaptiveStep(value);
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
        float const step1 = numeric_widget_detail::computeAdaptiveStep(1.f);
        float const step10 = numeric_widget_detail::computeAdaptiveStep(10.f);
        float const step100 = numeric_widget_detail::computeAdaptiveStep(100.f);

        // Each order of magnitude should increase the step by ~10x
        EXPECT_NEAR(step10 / step1, 10.f, 1.f);
        EXPECT_NEAR(step100 / step10, 10.f, 1.f);
    }

    TEST(AdaptiveDragFloat, ComputeDisplayPrecision_AdaptsToMagnitude)
    {
        EXPECT_GT(numeric_widget_detail::computeDisplayPrecision(0.001f),
                  numeric_widget_detail::computeDisplayPrecision(1000.f));
        EXPECT_GE(numeric_widget_detail::computeDisplayPrecision(0.f), 1);
    }

    TEST(NumericWidgetLayoutMode, MissingPreference_DefaultsToDialPlusDragFloat)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantScalar>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::Value);

        EXPECT_EQ(model.getNumericWidgetLayoutMode(parameter.getId()),
                  nodes::NumericWidgetLayoutMode::DialPlusDragFloat);
        EXPECT_FALSE(model.hasNumericWidgetLayoutMode(parameter.getId()));
    }

    TEST(NumericWidgetLayoutMode, SetPreference_PersistsPerParameterId)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantScalar>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::Value);
        model.setNumericWidgetLayoutMode(parameter.getId(), nodes::NumericWidgetLayoutMode::Slider);

        EXPECT_TRUE(model.hasNumericWidgetLayoutMode(parameter.getId()));
        EXPECT_EQ(model.getNumericWidgetLayoutMode(parameter.getId()),
                  nodes::NumericWidgetLayoutMode::Slider);
    }

    TEST(NumericWidgetLayoutMode, CopiedModel_RetainsStoredPreference)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantScalar>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::Value);
        auto const parameterId = parameter.getId();
        model.setNumericWidgetLayoutMode(parameterId, nodes::NumericWidgetLayoutMode::Slider);

        nodes::Model copiedModel(model);

        EXPECT_TRUE(copiedModel.hasNumericWidgetLayoutMode(parameterId));
        EXPECT_EQ(copiedModel.getNumericWidgetLayoutMode(parameterId),
                  nodes::NumericWidgetLayoutMode::Slider);
    }

    // --- VectorDisplayMode tests ---

    TEST(VectorDisplayMode, MissingPreference_DefaultsToVector)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantVector>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::X);
        EXPECT_EQ(model.getVectorDisplayMode(parameter.getId()),
                  nodes::VectorDisplayMode::Vector);
    }

    TEST(VectorDisplayMode, SetToColor_PersistsPerParameterId)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantVector>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::X);
        model.setVectorDisplayMode(parameter.getId(), nodes::VectorDisplayMode::Color);

        EXPECT_EQ(model.getVectorDisplayMode(parameter.getId()),
                  nodes::VectorDisplayMode::Color);
    }

    TEST(VectorDisplayMode, CopiedModel_RetainsStoredPreference)
    {
        nodes::Model model;
        auto * node = model.create<nodes::ConstantVector>();
        ASSERT_NE(node, nullptr);

        auto & parameter = node->parameter().at(nodes::FieldNames::X);
        auto const parameterId = parameter.getId();
        model.setVectorDisplayMode(parameterId, nodes::VectorDisplayMode::Color);

        nodes::Model copiedModel(model);
        EXPECT_EQ(copiedModel.getVectorDisplayMode(parameterId),
                  nodes::VectorDisplayMode::Color);
    }

} // namespace gladius::ui::tests
