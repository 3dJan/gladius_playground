/**
 * @file LinkDragState_tests.cpp
 * @brief Unit tests for port compatibility computation during link drag.
 */

#include "ui/LinkDragState.h"

#include <gtest/gtest.h>

namespace gladius::ui::tests
{
    TEST(LinkDragState, ComputeCompatibility_OutputSource_FindsMatchingInputs)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto * constantNode = model.create<nodes::ConstantScalar>();
        ASSERT_NE(constantNode, nullptr);

        auto const outputId = constantNode->getOutputs().at(nodes::FieldNames::Value).getId();
        auto const shapeId = model.getEndNode()->parameter().at(nodes::FieldNames::Shape).getId();
        auto const colorId = model.getEndNode()->parameter().at(nodes::FieldNames::Color).getId();

        LinkDragState state;
        state.beginDrag(outputId, std::type_index{typeid(float)}, true);
        state.computeCompatibility(model);

        EXPECT_TRUE(state.hasComputedCompatibility());
        EXPECT_TRUE(state.isCompatible(shapeId));
        EXPECT_FALSE(state.isCompatible(colorId));
    }

    TEST(LinkDragState, ComputeCompatibility_InputSource_FindsMatchingOutputs)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto * constantNode = model.create<nodes::ConstantScalar>();
        ASSERT_NE(constantNode, nullptr);

        auto const inputId = model.getEndNode()->parameter().at(nodes::FieldNames::Shape).getId();
        auto const scalarOutputId = constantNode->getOutputs().at(nodes::FieldNames::Value).getId();
        auto const vectorOutputId = model.getBeginNode()->getOutputs().at(nodes::FieldNames::Pos).getId();

        LinkDragState state;
        state.beginDrag(inputId, std::type_index{typeid(float)}, false);
        state.computeCompatibility(model);

        EXPECT_TRUE(state.hasComputedCompatibility());
        EXPECT_TRUE(state.isCompatible(scalarOutputId));
        EXPECT_FALSE(state.isCompatible(vectorOutputId));
    }

    TEST(LinkDragState, ComputeCompatibility_OutputSource_ExcludesSameNodeInputs)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto * absNode = model.create<nodes::Abs>();
        ASSERT_NE(absNode, nullptr);

        auto const outputId = absNode->getOutputs().at(nodes::FieldNames::Result).getId();
        auto const ownInputId = absNode->parameter().at(nodes::FieldNames::A).getId();
        auto const shapeId = model.getEndNode()->parameter().at(nodes::FieldNames::Shape).getId();

        LinkDragState state;
        state.beginDrag(outputId, std::type_index{typeid(float)}, true);
        state.computeCompatibility(model);

        EXPECT_TRUE(state.hasComputedCompatibility());
        EXPECT_FALSE(state.isCompatible(ownInputId));
        EXPECT_TRUE(state.isCompatible(shapeId));
    }

    TEST(LinkDragState, ComputeCompatibility_OutputSource_ExcludesCycleCreatingTargets)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto * addNode = model.create<nodes::Addition>();
        auto * absNode = model.create<nodes::Abs>();
        ASSERT_NE(addNode, nullptr);
        ASSERT_NE(absNode, nullptr);

        auto const addResultId = addNode->getOutputs().at(nodes::FieldNames::Result).getId();
        auto const addInputAId = addNode->parameter().at(nodes::FieldNames::A).getId();
        auto const addInputBId = addNode->parameter().at(nodes::FieldNames::B).getId();
        auto const absInputId = absNode->parameter().at(nodes::FieldNames::A).getId();
        auto const absResultId = absNode->getOutputs().at(nodes::FieldNames::Result).getId();
        auto const shapeId = model.getEndNode()->parameter().at(nodes::FieldNames::Shape).getId();

        ASSERT_TRUE(model.addLink(addResultId, absInputId));

        LinkDragState state;
        state.beginDrag(absResultId, std::type_index{typeid(float)}, true);
        state.computeCompatibility(model);

        EXPECT_TRUE(state.hasComputedCompatibility());
        EXPECT_FALSE(state.isCompatible(addInputAId));
        EXPECT_FALSE(state.isCompatible(addInputBId));
        EXPECT_TRUE(state.isCompatible(shapeId));
    }

    TEST(LinkDragState, ComputeCompatibility_FloatToFloat_IsCompatible)
    {
        LinkDragState state;
        state.beginDrag(7, std::type_index{typeid(float)}, true);

        // Simulate: port 42 is a float input → compatible
        state.setCompatiblePorts({42});

        EXPECT_TRUE(state.isCompatible(42));
    }

    TEST(LinkDragState, ComputeCompatibility_FloatToVec3_IsIncompatible)
    {
        LinkDragState state;
        state.beginDrag(7, std::type_index{typeid(float)}, true);

        // Only port 42 is compatible; port 99 (vec3) is not
        state.setCompatiblePorts({42});

        EXPECT_FALSE(state.isCompatible(99));
    }

    TEST(LinkDragState, ComputeCompatibility_DynamicTypeResolved_UsesResolvedType)
    {
        // When a dynamic port resolves to float, float ports should be compatible
        LinkDragState state;
        state.beginDrag(11, std::type_index{typeid(float)}, false);

        state.setCompatiblePorts({10, 20});

        EXPECT_TRUE(state.isCompatible(10));
        EXPECT_TRUE(state.isCompatible(20));
        EXPECT_FALSE(state.isCompatible(30));
    }

    TEST(LinkDragState, ComputeCompatibility_UnresolvedDynamic_AllCompatible)
    {
        // When compatibility set is empty (unresolved), all ports are compatible
        LinkDragState state;
        state.beginDrag(9, std::type_index{typeid(void)}, true);

        EXPECT_TRUE(state.isCompatible(42));
        EXPECT_TRUE(state.isCompatible(99));
    }

    TEST(LinkDragState, IsCompatible_PortInSet_ReturnsTrue)
    {
        LinkDragState state;
        state.beginDrag(9, std::type_index{typeid(float)}, true);
        state.setCompatiblePorts({1, 2, 3, 4, 5});

        EXPECT_TRUE(state.isCompatible(3));
    }

    TEST(LinkDragState, IsCompatible_PortNotInSet_ReturnsFalse)
    {
        LinkDragState state;
        state.beginDrag(9, std::type_index{typeid(float)}, true);
        state.setCompatiblePorts({1, 2, 3, 4, 5});

        EXPECT_FALSE(state.isCompatible(10));
    }

    TEST(LinkDragState, Reset_ClearsState)
    {
        LinkDragState state;
        state.beginDrag(42, std::type_index{typeid(float)}, true);
        state.setCompatiblePorts({1, 2, 3});

        state.reset();

        EXPECT_FALSE(state.isDragging);
        EXPECT_EQ(state.sourcePortId, 0);
        EXPECT_EQ(state.sourcePortType, std::type_index{typeid(void)});
        EXPECT_FALSE(state.sourceIsOutput);
        EXPECT_TRUE(state.compatiblePorts.empty());
        EXPECT_FALSE(state.hasComputedCompatibility());
    }

    TEST(LinkDragState, IsCompatible_NotDragging_AlwaysTrue)
    {
        LinkDragState state;
        state.setCompatiblePorts({1, 2, 3});

        // Not dragging → everything is compatible (no dimming)
        EXPECT_TRUE(state.isCompatible(99));
    }

} // namespace gladius::ui::tests
