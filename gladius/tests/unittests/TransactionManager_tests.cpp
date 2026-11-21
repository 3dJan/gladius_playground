#include "nodes/History.h"
#include "tools/CheckpointManager.h"
#include "tools/TransactionManager.h"

#include <gtest/gtest.h>

namespace gladius::tools::tests
{
    TEST(TransactionManager, CommitEdit_WithModifiedAssembly_RecordsSingleUndoEntry)
    {
        // Arrange
        nodes::Assembly assembly;
        nodes::History history;
        TransactionManager tx([&](nodes::Assembly const & preEdit, std::string const & desc) {
            history.storeState(preEdit, desc);
        });

        // Act
        ASSERT_TRUE(tx.beginEdit("Add function", assembly));
        EXPECT_TRUE(tx.isActive());
        assembly.addModelIfNotExisting(42);
        ASSERT_TRUE(tx.commitEdit(assembly));

        // Assert
        EXPECT_FALSE(tx.isActive());
        EXPECT_TRUE(history.canUnDo());
        EXPECT_EQ(assembly.getFunctions().count(42), 1u);

        history.undo(&assembly);
        EXPECT_EQ(assembly.getFunctions().count(42), 0u);
    }

    TEST(TransactionManager, RollbackEdit_WithModifiedAssembly_RestoresPreEditState)
    {
        // Arrange
        nodes::Assembly assembly;
        nodes::History history;
        TransactionManager tx([&](nodes::Assembly const & preEdit, std::string const & desc) {
            history.storeState(preEdit, desc);
        });

        ASSERT_TRUE(tx.beginEdit("Experiment", assembly));
        assembly.addModelIfNotExisting(99);

        // Act
        ASSERT_TRUE(tx.rollbackEdit(assembly));

        // Assert
        EXPECT_FALSE(tx.isActive());
        EXPECT_FALSE(history.canUnDo());
        EXPECT_EQ(assembly.getFunctions().count(99), 0u);
    }

    TEST(CheckpointManager, CreateRestoreAndCompare_ValidAssembly_TracksDiffsAndRestores)
    {
        // Arrange
        nodes::Assembly assembly;
        CheckpointManager manager;
        std::string const cp0 = manager.createCheckpoint("Initial", 0, assembly);

        // Act
        assembly.addModelIfNotExisting(7);
        auto const diff = manager.compareCheckpoint(cp0, assembly);

        // Assert
        EXPECT_TRUE(diff.value("success", false));
        ASSERT_EQ(diff["addedFunctionIds"].size(), 1u);
        EXPECT_EQ(diff["addedFunctionIds"][0].get<nodes::ResourceId>(), 7u);

        // Act restore
        ASSERT_TRUE(manager.restoreCheckpoint(cp0, assembly));
        EXPECT_EQ(assembly.getFunctions().count(7), 0u);
    }
} // namespace gladius::tools::tests
