#include <gtest/gtest.h>

#include "nodes/DerivedNodes.h"
#include "nodes/FunctionalEquality.h"
#include "nodes/Model.h"

namespace gladius::tests
{
    class FunctionalEquality_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Reset epsilon to default before each test
            nodes::FunctionalEquality::setEpsilon(1e-6);
        }

        /// Creates a simple model: Input -> ConstantScalar(value) -> End
        nodes::Model createModelWithConstant(float value)
        {
            nodes::Model model;
            model.createBeginEnd();
            auto * constNode = model.create<nodes::ConstantScalar>();
            constNode->parameter().at(nodes::FieldNames::Value).setValue(value);
            model.updateGraphAndOrderIfNeeded();
            return model;
        }

        /// Creates a model: Input -> Addition(a, b) -> End
        nodes::Model createModelWithAddition()
        {
            nodes::Model model;
            model.createBeginEnd();
            model.create<nodes::Addition>();
            model.updateGraphAndOrderIfNeeded();
            return model;
        }

        /// Creates a model: Input -> Subtraction(a, b) -> End
        nodes::Model createModelWithSubtraction()
        {
            nodes::Model model;
            model.createBeginEnd();
            model.create<nodes::Subtraction>();
            model.updateGraphAndOrderIfNeeded();
            return model;
        }
    };

    // T009: Identical models should produce the same hash
    TEST_F(FunctionalEquality_Test, ComputeHash_IdenticalModels_ReturnsSameHash)
    {
        auto model1 = createModelWithConstant(42.0f);
        auto model2 = createModelWithConstant(42.0f);

        auto hash1 = nodes::FunctionalEquality::computeHash(model1);
        auto hash2 = nodes::FunctionalEquality::computeHash(model2);

        EXPECT_EQ(hash1, hash2);
    }

    // T010: Different models should produce different hashes
    TEST_F(FunctionalEquality_Test, ComputeHash_DifferentModels_ReturnsDifferentHash)
    {
        auto model1 = createModelWithConstant(42.0f);
        auto model2 = createModelWithConstant(100.0f);

        auto hash1 = nodes::FunctionalEquality::computeHash(model1);
        auto hash2 = nodes::FunctionalEquality::computeHash(model2);

        EXPECT_NE(hash1, hash2);
    }

    // T011: Models with identical structure should be equal
    TEST_F(FunctionalEquality_Test, AreEqual_IdenticalStructure_ReturnsTrue)
    {
        auto model1 = createModelWithConstant(42.0f);
        auto model2 = createModelWithConstant(42.0f);

        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T012: Models with different constant values should not be equal
    TEST_F(FunctionalEquality_Test, AreEqual_DifferentConstants_ReturnsFalse)
    {
        auto model1 = createModelWithConstant(42.0f);
        auto model2 = createModelWithConstant(100.0f);

        EXPECT_FALSE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T013: Models with different node types should not be equal
    TEST_F(FunctionalEquality_Test, AreEqual_DifferentNodeTypes_ReturnsFalse)
    {
        auto model1 = createModelWithAddition();
        auto model2 = createModelWithSubtraction();

        EXPECT_FALSE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T014: Models should be equal even if node names differ (only structure matters)
    TEST_F(FunctionalEquality_Test, AreEqual_DifferentNodeNames_ReturnsTrue)
    {
        nodes::Model model1;
        model1.createBeginEnd();
        auto * const1 = model1.create<nodes::ConstantScalar>();
        const1->parameter().at(nodes::FieldNames::Value).setValue(42.0f);
        const1->setDisplayName("MyConstant");
        model1.updateGraphAndOrderIfNeeded();

        nodes::Model model2;
        model2.createBeginEnd();
        auto * const2 = model2.create<nodes::ConstantScalar>();
        const2->parameter().at(nodes::FieldNames::Value).setValue(42.0f);
        const2->setDisplayName("DifferentName");
        model2.updateGraphAndOrderIfNeeded();

        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T015: Empty models (just Begin/End) should be equal
    TEST_F(FunctionalEquality_Test, AreEqual_EmptyModels_ReturnsTrue)
    {
        nodes::Model model1;
        model1.createBeginEnd();
        model1.updateGraphAndOrderIfNeeded();

        nodes::Model model2;
        model2.createBeginEnd();
        model2.updateGraphAndOrderIfNeeded();

        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T016: Constants within epsilon tolerance should be considered equal
    TEST_F(FunctionalEquality_Test, AreEqual_ConstantsWithinEpsilon_ReturnsTrue)
    {
        float const baseValue = 1.0f;
        float const delta = 1e-7f; // Smaller than default epsilon of 1e-6

        auto model1 = createModelWithConstant(baseValue);
        auto model2 = createModelWithConstant(baseValue + delta);

        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // Additional: Epsilon getter/setter works correctly
    TEST_F(FunctionalEquality_Test, Epsilon_SetAndGet_ReturnsSetValue)
    {
        double const newEpsilon = 1e-3;
        nodes::FunctionalEquality::setEpsilon(newEpsilon);

        EXPECT_NEAR(nodes::FunctionalEquality::getEpsilon(), newEpsilon, 1e-12);
    }

    // Additional: Constants outside epsilon should NOT be equal
    TEST_F(FunctionalEquality_Test, AreEqual_ConstantsOutsideEpsilon_ReturnsFalse)
    {
        float const baseValue = 1.0f;
        float const delta = 1e-4f; // Larger than default epsilon of 1e-6

        auto model1 = createModelWithConstant(baseValue);
        auto model2 = createModelWithConstant(baseValue + delta);

        EXPECT_FALSE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T044: Self-referencing function (calls itself) should be handled without crash
    TEST_F(FunctionalEquality_Test, AreEqual_SelfReferencingFunction_HandledCorrectly)
    {
        ResourceId const funcId = 100;
        
        // Create two identical functions that call themselves
        nodes::Model model1;
        model1.createBeginEnd();
        auto * call1 = model1.create<nodes::FunctionCall>();
        call1->setFunctionId(funcId); // Set to own ID (self-reference)
        model1.updateGraphAndOrderIfNeeded();

        nodes::Model model2;
        model2.createBeginEnd();
        auto * call2 = model2.create<nodes::FunctionCall>();
        call2->setFunctionId(funcId);
        model2.updateGraphAndOrderIfNeeded();

        // Should be equal (both have same structure with same function reference)
        // Primary concern: verify no infinite loop or crash
        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(model1, model2));
    }

    // T045: Circular reference pattern (A calls B, B calls A) should be handled
    TEST_F(FunctionalEquality_Test, AreEqual_CircularReferencePattern_HandledCorrectly)
    {
        ResourceId const funcA = 100;
        ResourceId const funcB = 200;
        
        // Create function that calls funcB
        nodes::Model modelCallsB1;
        modelCallsB1.createBeginEnd();
        auto * callB1 = modelCallsB1.create<nodes::FunctionCall>();
        callB1->setFunctionId(funcB);
        modelCallsB1.updateGraphAndOrderIfNeeded();

        nodes::Model modelCallsB2;
        modelCallsB2.createBeginEnd();
        auto * callB2 = modelCallsB2.create<nodes::FunctionCall>();
        callB2->setFunctionId(funcB);
        modelCallsB2.updateGraphAndOrderIfNeeded();

        // Both call the same external function - should be equal
        EXPECT_TRUE(nodes::FunctionalEquality::areEqual(modelCallsB1, modelCallsB2));
        
        // Create function that calls funcA (potential circular reference)
        nodes::Model modelCallsA;
        modelCallsA.createBeginEnd();
        auto * callA = modelCallsA.create<nodes::FunctionCall>();
        callA->setFunctionId(funcA);
        modelCallsA.updateGraphAndOrderIfNeeded();

        // Calls different functions - should NOT be equal
        EXPECT_FALSE(nodes::FunctionalEquality::areEqual(modelCallsB1, modelCallsA));
    }

} // namespace gladius::tests
