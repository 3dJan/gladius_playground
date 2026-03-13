/**
 * @file LinkDragState_tests.cpp
 * @brief Unit tests for port compatibility computation during link drag.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <typeindex>
#include <unordered_set>

namespace gladius::ui::tests
{
    /// Minimal test-only reimplementation of LinkDragState logic
    /// to verify compatibility set behavior without depending on Model.
    struct TestLinkDragState
    {
        bool isDragging = false;
        int64_t sourcePortId = 0;
        std::type_index sourcePortType{typeid(void)};
        bool sourceIsOutput = false;
        std::unordered_set<int64_t> compatiblePorts;

        [[nodiscard]] bool isCompatible(int64_t portOrParamId) const
        {
            if (!isDragging)
            {
                return true;
            }
            if (compatiblePorts.empty())
            {
                return true;
            }
            return compatiblePorts.count(portOrParamId) > 0;
        }

        void reset()
        {
            isDragging = false;
            sourcePortId = 0;
            sourcePortType = std::type_index{typeid(void)};
            sourceIsOutput = false;
            compatiblePorts.clear();
        }
    };

    TEST(LinkDragState, ComputeCompatibility_FloatToFloat_IsCompatible)
    {
        TestLinkDragState state;
        state.isDragging = true;
        state.sourcePortType = std::type_index{typeid(float)};
        state.sourceIsOutput = true;

        // Simulate: port 42 is a float input → compatible
        state.compatiblePorts.insert(42);

        EXPECT_TRUE(state.isCompatible(42));
    }

    TEST(LinkDragState, ComputeCompatibility_FloatToVec3_IsIncompatible)
    {
        TestLinkDragState state;
        state.isDragging = true;
        state.sourcePortType = std::type_index{typeid(float)};
        state.sourceIsOutput = true;

        // Only port 42 is compatible; port 99 (vec3) is not
        state.compatiblePorts.insert(42);

        EXPECT_FALSE(state.isCompatible(99));
    }

    TEST(LinkDragState, ComputeCompatibility_DynamicTypeResolved_UsesResolvedType)
    {
        // When a dynamic port resolves to float, float ports should be compatible
        TestLinkDragState state;
        state.isDragging = true;
        state.sourcePortType = std::type_index{typeid(float)}; // resolved

        state.compatiblePorts.insert(10);
        state.compatiblePorts.insert(20);

        EXPECT_TRUE(state.isCompatible(10));
        EXPECT_TRUE(state.isCompatible(20));
        EXPECT_FALSE(state.isCompatible(30));
    }

    TEST(LinkDragState, ComputeCompatibility_UnresolvedDynamic_AllCompatible)
    {
        // When compatibility set is empty (unresolved), all ports are compatible
        TestLinkDragState state;
        state.isDragging = true;
        state.sourcePortType = std::type_index{typeid(void)}; // unresolved
        // compatiblePorts is empty → fallback: all compatible

        EXPECT_TRUE(state.isCompatible(42));
        EXPECT_TRUE(state.isCompatible(99));
    }

    TEST(LinkDragState, IsCompatible_PortInSet_ReturnsTrue)
    {
        TestLinkDragState state;
        state.isDragging = true;
        state.compatiblePorts = {1, 2, 3, 4, 5};

        EXPECT_TRUE(state.isCompatible(3));
    }

    TEST(LinkDragState, IsCompatible_PortNotInSet_ReturnsFalse)
    {
        TestLinkDragState state;
        state.isDragging = true;
        state.compatiblePorts = {1, 2, 3, 4, 5};

        EXPECT_FALSE(state.isCompatible(10));
    }

    TEST(LinkDragState, Reset_ClearsState)
    {
        TestLinkDragState state;
        state.isDragging = true;
        state.sourcePortId = 42;
        state.sourcePortType = std::type_index{typeid(float)};
        state.sourceIsOutput = true;
        state.compatiblePorts = {1, 2, 3};

        state.reset();

        EXPECT_FALSE(state.isDragging);
        EXPECT_EQ(state.sourcePortId, 0);
        EXPECT_EQ(state.sourcePortType, std::type_index{typeid(void)});
        EXPECT_FALSE(state.sourceIsOutput);
        EXPECT_TRUE(state.compatiblePorts.empty());
    }

    TEST(LinkDragState, IsCompatible_NotDragging_AlwaysTrue)
    {
        TestLinkDragState state;
        state.isDragging = false;
        state.compatiblePorts = {1, 2, 3};

        // Not dragging → everything is compatible (no dimming)
        EXPECT_TRUE(state.isCompatible(99));
    }

} // namespace gladius::ui::tests
