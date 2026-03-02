#include "nodes/FunctionDeduplicator.h"

#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/FunctionalEquality.h"
#include "nodes/History.h"
#include "nodes/Model.h"

#include <gtest/gtest.h>

#include <memory>

namespace gladius::tests
{
    using namespace gladius::nodes;

    class FunctionDeduplicator_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create a minimal assembly with assembly model
            m_assembly = std::make_unique<Assembly>();
            m_assembly->addModelIfNotExisting(ASSEMBLY_MODEL_ID);
            m_assembly->setAssemblyModelId(ASSEMBLY_MODEL_ID);
        }

        /// Create a function model with a ConstantScalar node and add to assembly
        ResourceId addFunctionWithConstant(float value)
        {
            auto const newId = m_nextFunctionId++;
            m_assembly->addModelIfNotExisting(newId);

            auto model = m_assembly->findModel(newId);
            if (model)
            {
                model->createBeginEnd();
                auto * constNode = model->create<ConstantScalar>();
                constNode->parameter().at(FieldNames::Value).setValue(value);
                model->updateGraphAndOrderIfNeeded();
            }
            return newId;
        }

        /// Create a function model with a FunctionCall node referencing another function
        ResourceId addFunctionWithCall(ResourceId referencedFunction)
        {
            auto const newId = m_nextFunctionId++;
            m_assembly->addModelIfNotExisting(newId);

            auto model = m_assembly->findModel(newId);
            if (model)
            {
                model->createBeginEnd();
                auto * callNode = model->create<FunctionCall>();
                callNode->setFunctionId(referencedFunction);
                model->updateGraphAndOrderIfNeeded();
            }
            return newId;
        }

        /// Create a function model with a FunctionGradient node referencing another function
        ResourceId addFunctionWithGradient(ResourceId referencedFunction)
        {
            auto const newId = m_nextFunctionId++;
            m_assembly->addModelIfNotExisting(newId);

            auto model = m_assembly->findModel(newId);
            if (model)
            {
                model->createBeginEnd();
                auto * gradNode = model->create<FunctionGradient>();
                gradNode->setFunctionId(referencedFunction);
                model->updateGraphAndOrderIfNeeded();
            }
            return newId;
        }

        static constexpr ResourceId ASSEMBLY_MODEL_ID = 1;
        std::unique_ptr<Assembly> m_assembly;
        ResourceId m_nextFunctionId = 100; // Start high to avoid collision with assembly model
    };

    // ============================================================
    // T022: findDuplicateGroups with no duplicates returns empty
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, FindDuplicateGroups_NoDuplicates_ReturnsEmpty)
    {
        // Add two different functions (different constant values)
        addFunctionWithConstant(1.0f);
        addFunctionWithConstant(2.0f);

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);

        EXPECT_TRUE(groups.empty());
    }

    // ============================================================
    // T023: findDuplicateGroups with one pair returns one group
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, FindDuplicateGroups_OnePair_ReturnsOneGroup)
    {
        // Add two identical functions
        auto const id1 = addFunctionWithConstant(42.0f);
        auto const id2 = addFunctionWithConstant(42.0f);

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);

        ASSERT_EQ(groups.size(), 1);
        EXPECT_EQ(groups[0].members.size(), 2);
        // Members should include both IDs
        EXPECT_TRUE(std::find(groups[0].members.begin(), groups[0].members.end(), id1) !=
                    groups[0].members.end());
        EXPECT_TRUE(std::find(groups[0].members.begin(), groups[0].members.end(), id2) !=
                    groups[0].members.end());
    }

    // ============================================================
    // T024: findDuplicateGroups with multiple pairs returns multiple groups
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, FindDuplicateGroups_MultiplePairs_ReturnsMultipleGroups)
    {
        // Pair 1: two functions with constant 1.0
        addFunctionWithConstant(1.0f);
        addFunctionWithConstant(1.0f);

        // Pair 2: two functions with constant 2.0
        addFunctionWithConstant(2.0f);
        addFunctionWithConstant(2.0f);

        // Unique function
        addFunctionWithConstant(3.0f);

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);

        EXPECT_EQ(groups.size(), 2);
    }

    // ============================================================
    // T025: findDuplicateGroups excludes assembly model
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, FindDuplicateGroups_AssemblyModelExcluded_NeverInGroup)
    {
        // Create a function that's identical to the assembly model's structure
        // (Both have Begin+End with no additional nodes)
        auto assemblyModel = m_assembly->findModel(ASSEMBLY_MODEL_ID);
        assemblyModel->createBeginEnd();
        assemblyModel->updateGraphAndOrderIfNeeded();

        // Add a function with same structure as assembly model
        m_assembly->addModelIfNotExisting(200);
        auto dupModel = m_assembly->findModel(200);
        dupModel->createBeginEnd();
        dupModel->updateGraphAndOrderIfNeeded();

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);

        // Should not find assembly model as duplicate even if structurally identical
        for (auto const & group : groups)
        {
            EXPECT_TRUE(std::find(group.members.begin(), group.members.end(), ASSEMBLY_MODEL_ID) ==
                        group.members.end())
                << "Assembly model should never be in a duplicate group";
        }
    }

    // ============================================================
    // T026: deduplicate removes duplicates and returns correct count
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, Deduplicate_RemovesDuplicates_ReturnsCorrectCount)
    {
        // Add three identical functions (two should be removed)
        addFunctionWithConstant(42.0f);
        addFunctionWithConstant(42.0f);
        addFunctionWithConstant(42.0f);

        auto const initialFunctionCount = m_assembly->getFunctions().size();

        auto const result = FunctionDeduplicator::deduplicate(*m_assembly);

        EXPECT_EQ(result.removedCount, 2);
        EXPECT_EQ(m_assembly->getFunctions().size(), initialFunctionCount - 2);
    }

    // ============================================================
    // T027: deduplicate updates FunctionCall references
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, Deduplicate_UpdatesFunctionCallReferences)
    {
        // Create two identical functions
        auto const original = addFunctionWithConstant(100.0f);
        auto const duplicate = addFunctionWithConstant(100.0f);

        // Create a function that calls the duplicate
        auto const caller = addFunctionWithCall(duplicate);

        auto const result = FunctionDeduplicator::deduplicate(*m_assembly);

        // One duplicate should be removed
        EXPECT_EQ(result.removedCount, 1);

        // Find the FunctionCall node in caller and verify it now references a valid function
        auto callerModel = m_assembly->findModel(caller);
        ASSERT_NE(callerModel, nullptr);

        bool foundUpdatedCall = false;
        for (auto const & [nodeId, node] : *callerModel)
        {
            if (auto * callNode = dynamic_cast<FunctionCall *>(node.get()))
            {
                // Reference should be updated to canonical (the one that was kept)
                // With smart selection: duplicate has 1 reference (from caller), original has 0
                // So duplicate should be kept as canonical
                auto const referencedId = callNode->getFunctionId();
                auto const stillExists = m_assembly->findModel(referencedId) != nullptr;
                EXPECT_TRUE(stillExists)
                    << "FunctionCall should reference a function that still exists";
                foundUpdatedCall = true;
            }
        }
        EXPECT_TRUE(foundUpdatedCall) << "Should have a FunctionCall node in the caller function";
    }

    // ============================================================
    // T028: deduplicate updates FunctionGradient references
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, Deduplicate_UpdatesFunctionGradientReferences)
    {
        // Create two identical functions
        auto const original = addFunctionWithConstant(200.0f);
        auto const duplicate = addFunctionWithConstant(200.0f);

        // Create a function that uses FunctionGradient referencing the duplicate
        auto const caller = addFunctionWithGradient(duplicate);

        auto const result = FunctionDeduplicator::deduplicate(*m_assembly);

        // One duplicate should be removed
        EXPECT_EQ(result.removedCount, 1);

        // Find the FunctionGradient node in caller and verify it now references a valid function
        auto callerModel = m_assembly->findModel(caller);
        ASSERT_NE(callerModel, nullptr);

        bool foundUpdatedGradient = false;
        for (auto const & [nodeId, node] : *callerModel)
        {
            if (auto * gradNode = dynamic_cast<FunctionGradient *>(node.get()))
            {
                // Reference should be updated to canonical (the one that was kept)
                // With smart selection: duplicate has 1 reference (from caller), original has 0
                // So duplicate should be kept as canonical
                auto const referencedId = gradNode->getFunctionId();
                auto const stillExists = m_assembly->findModel(referencedId) != nullptr;
                EXPECT_TRUE(stillExists)
                    << "FunctionGradient should reference a function that still exists";
                foundUpdatedGradient = true;
            }
        }
        EXPECT_TRUE(foundUpdatedGradient)
            << "Should have a FunctionGradient node in the caller function";
    }

    // ============================================================
    // T035: selectCanonical prefers function with more internal references
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, SelectCanonical_NoExternalRefs_HigherInternalCount_SelectsHigher)
    {
        // Create two identical functions
        auto const lessReferenced = addFunctionWithConstant(42.0f);
        auto const moreReferenced = addFunctionWithConstant(42.0f);

        // Helper to create unique callers (each with a different constant value)
        auto addUniqueCallerWithConstant = [this](ResourceId referencedFunction, float uniqueValue) {
            auto const newId = m_nextFunctionId++;
            m_assembly->addModelIfNotExisting(newId);
            auto model = m_assembly->findModel(newId);
            if (model)
            {
                model->createBeginEnd();
                auto * callNode = model->create<FunctionCall>();
                callNode->setFunctionId(referencedFunction);
                auto * constNode = model->create<ConstantScalar>();
                constNode->parameter().at(FieldNames::Value).setValue(uniqueValue);
                model->updateGraphAndOrderIfNeeded();
            }
            return newId;
        };

        // Create multiple unique functions that call moreReferenced
        addUniqueCallerWithConstant(moreReferenced, 1.0f);
        addUniqueCallerWithConstant(moreReferenced, 2.0f);
        addUniqueCallerWithConstant(moreReferenced, 3.0f);

        // Create only one unique function that calls lessReferenced
        addUniqueCallerWithConstant(lessReferenced, 4.0f);

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);
        ASSERT_EQ(groups.size(), 1)
            << "Should only have one duplicate group (the two identical 42.0f functions)";

        auto const canonical = FunctionDeduplicator::selectCanonical(groups[0], *m_assembly);

        // Should select the one with more references
        EXPECT_EQ(canonical, moreReferenced);
    }

    // ============================================================
    // T036: selectCanonical uses lower resource ID as tie-breaker
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, SelectCanonical_EqualRefCounts_SelectsLowerResourceId)
    {
        // Create two identical functions with no references
        auto const firstId = addFunctionWithConstant(42.0f);
        auto const secondId = addFunctionWithConstant(42.0f);

        auto const groups = FunctionDeduplicator::findDuplicateGroups(*m_assembly);
        ASSERT_EQ(groups.size(), 1);

        auto const canonical = FunctionDeduplicator::selectCanonical(groups[0], *m_assembly);

        // Should select the one with lower resource ID
        EXPECT_EQ(canonical, std::min(firstId, secondId));
    }

    // ============================================================
    // T037: deduplicate integrates selectCanonical correctly
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, Deduplicate_UsesSelectCanonical_KeepsMoreReferenced)
    {
        // Create two identical functions
        auto const lessReferenced = addFunctionWithConstant(42.0f);
        auto const moreReferenced = addFunctionWithConstant(42.0f);

        // Helper to create unique callers (each with a different constant value)
        auto addUniqueCallerWithConstant = [this](ResourceId referencedFunction, float uniqueValue) {
            auto const newId = m_nextFunctionId++;
            m_assembly->addModelIfNotExisting(newId);
            auto model = m_assembly->findModel(newId);
            if (model)
            {
                model->createBeginEnd();
                auto * callNode = model->create<FunctionCall>();
                callNode->setFunctionId(referencedFunction);
                auto * constNode = model->create<ConstantScalar>();
                constNode->parameter().at(FieldNames::Value).setValue(uniqueValue);
                model->updateGraphAndOrderIfNeeded();
            }
            return newId;
        };

        // Create multiple unique callers for moreReferenced
        addUniqueCallerWithConstant(moreReferenced, 1.0f);
        addUniqueCallerWithConstant(moreReferenced, 2.0f);

        // Create one unique caller for lessReferenced
        addUniqueCallerWithConstant(lessReferenced, 3.0f);

        auto const result = FunctionDeduplicator::deduplicate(*m_assembly);

        // One duplicate should be removed
        EXPECT_EQ(result.removedCount, 1);

        // The more referenced function should still exist
        EXPECT_NE(m_assembly->findModel(moreReferenced), nullptr);

        // The less referenced function should be removed
        EXPECT_EQ(m_assembly->findModel(lessReferenced), nullptr);
    }

    // ============================================================
    // T041: Deduplicate can be undone via History
    // ============================================================
    TEST_F(FunctionDeduplicator_Test, Deduplicate_WithHistory_CanUndo)
    {
        auto const functionCountBefore = m_assembly->getFunctions().size();
        
        // Create two identical functions
        auto const original = addFunctionWithConstant(42.0f);
        auto const duplicate = addFunctionWithConstant(42.0f);

        auto const functionCountAfterAdd = m_assembly->getFunctions().size();
        ASSERT_EQ(functionCountAfterAdd, functionCountBefore + 2);

        // Store state before deduplication
        History history;
        history.storeState(*m_assembly, "Before deduplication");

        // Perform deduplication
        auto const result = FunctionDeduplicator::deduplicate(*m_assembly);
        EXPECT_EQ(result.removedCount, 1);
        EXPECT_EQ(m_assembly->getFunctions().size(), functionCountAfterAdd - 1);

        // Verify undo is possible
        EXPECT_TRUE(history.canUnDo());

        // Perform undo
        history.undo(m_assembly.get());

        // Verify both functions are restored
        EXPECT_EQ(m_assembly->getFunctions().size(), functionCountAfterAdd);
        EXPECT_NE(m_assembly->findModel(original), nullptr);
        EXPECT_NE(m_assembly->findModel(duplicate), nullptr);
    }

} // namespace gladius::tests
