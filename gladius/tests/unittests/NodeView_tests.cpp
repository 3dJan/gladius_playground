#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "../../src/ui/NodeView.h"
#include "../../src/ui/PinHitboxGeometry.h"
#include "../../src/nodes/Model.h"
#include "../../src/nodes/NodeBase.h"
#include "../../src/ui/ModelEditor.h"

namespace gladius::ui::tests
{
    /// Test fixture for NodeView tag replacement functionality
    class NodeViewTagReplacementTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_model = std::make_shared<nodes::Model>();
            m_nodeView = std::make_unique<NodeView>();
            
            // Create test nodes with tags using the create() method
            auto* node1 = m_model->create<nodes::ConstantScalar>();
            node1->setTag("test_group");
            m_node1Id = node1->getId();
            
            auto* node2 = m_model->create<nodes::ConstantScalar>();
            node2->setTag("test_group");
            m_node2Id = node2->getId();
            
            auto* node3 = m_model->create<nodes::ConstantScalar>();
            node3->setTag("other_group");
            m_node3Id = node3->getId();
            
            m_nodeView->setCurrentModel(m_model);
            m_nodeView->updateNodeGroups();
        }

        void TearDown() override
        {
            m_nodeView.reset();
            m_model.reset();
        }

        std::shared_ptr<nodes::Model> m_model;
        std::unique_ptr<NodeView> m_nodeView;
        nodes::NodeId m_node1Id;
        nodes::NodeId m_node2Id;
        nodes::NodeId m_node3Id;
    };

    /// Test successful tag replacement for a group
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_WithValidParameters_ReplacesAllNodesInGroup)
    {
        // Arrange
        std::string const oldTag = "test_group";
        std::string const newTag = "renamed_group";

        // Act
        bool const result = m_nodeView->replaceGroupTag(oldTag, newTag);

        // Assert
        EXPECT_TRUE(result);
        
        // Verify that both nodes in the group have the new tag
        auto node1 = m_model->getNode(m_node1Id);
        auto node2 = m_model->getNode(m_node2Id);
        auto node3 = m_model->getNode(m_node3Id);
        
        ASSERT_TRUE(node1.has_value());
        ASSERT_TRUE(node2.has_value());
        ASSERT_TRUE(node3.has_value());
        
        EXPECT_EQ(node1.value()->getTag(), newTag);
        EXPECT_EQ(node2.value()->getTag(), newTag);
        EXPECT_EQ(node3.value()->getTag(), "other_group"); // Should remain unchanged
        
        // Verify group structure is updated
        EXPECT_FALSE(m_nodeView->hasGroup(oldTag));
        EXPECT_TRUE(m_nodeView->hasGroup(newTag));
    }

    /// Test that replacement fails with empty old tag
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_WithEmptyOldTag_ReturnsFalse)
    {
        // Act
        bool const result = m_nodeView->replaceGroupTag("", "new_tag");

        // Assert
        EXPECT_FALSE(result);
    }

    /// Test that replacement fails with empty new tag
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_WithEmptyNewTag_ReturnsFalse)
    {
        // Act
        bool const result = m_nodeView->replaceGroupTag("test_group", "");

        // Assert
        EXPECT_FALSE(result);
    }

    /// Test that replacement fails when old and new tags are the same
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_WithSameTags_ReturnsFalse)
    {
        // Act
        bool const result = m_nodeView->replaceGroupTag("test_group", "test_group");

        // Assert
        EXPECT_FALSE(result);
    }

    /// Test that replacement fails when old tag doesn't exist
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_WithNonExistentOldTag_ReturnsFalse)
    {
        // Act
        bool const result = m_nodeView->replaceGroupTag("non_existent", "new_tag");

        // Assert
        EXPECT_FALSE(result);
    }

    /// Test that group structure is correctly updated after tag replacement
    TEST_F(NodeViewTagReplacementTest, ReplaceGroupTag_AfterReplacement_UpdatesGroupStructure)
    {
        // Arrange
        std::string const oldTag = "test_group";
        std::string const newTag = "updated_group";
        
        // Verify initial state
        EXPECT_TRUE(m_nodeView->hasGroup(oldTag));
        EXPECT_FALSE(m_nodeView->hasGroup(newTag));

        // Act
        bool const result = m_nodeView->replaceGroupTag(oldTag, newTag);

        // Assert
        EXPECT_TRUE(result);
        EXPECT_FALSE(m_nodeView->hasGroup(oldTag));
        EXPECT_TRUE(m_nodeView->hasGroup(newTag));
    }

    TEST(PinHitboxGeometryTest, InputPin_UsesMinimumHitTargetAndSeparatesMenuButton)
    {
        ImVec2 const cellCursor{100.f, 200.f};
        auto const geometry = makePinHitboxGeometry(cellCursor, 48.f, 20.f, 1.f, true);

        EXPECT_FLOAT_EQ(geometry.hitRectMax.x - geometry.hitRectMin.x, 24.f);
        EXPECT_FLOAT_EQ(geometry.hitRectMax.y - geometry.hitRectMin.y, 24.f);
        EXPECT_FLOAT_EQ(geometry.markerCenter.x, 112.f);
        EXPECT_FLOAT_EQ(geometry.markerCenter.y, 210.f);
        EXPECT_FLOAT_EQ(geometry.pivotRectMax.x - geometry.pivotRectMin.x, 8.f);
        EXPECT_LE(geometry.hitRectMax.x, geometry.menuButtonPosition.x);
        EXPECT_FLOAT_EQ(geometry.menuButtonPosition.x, 128.f);
        EXPECT_FLOAT_EQ(pinMinimumRowHeight(1.f), 26.f);
        EXPECT_FLOAT_EQ(pinRailWidth(20.f, 5.f, 1.f), 58.f);
    }

    TEST(PinHitboxGeometryTest, OutputPin_AnchorsAtOuterEdgeAndPlacesMenuInside)
    {
        ImVec2 const cellCursor{100.f, 200.f};
        auto const geometry = makePinHitboxGeometry(cellCursor, 48.f, 20.f, 1.f, false);

        EXPECT_FLOAT_EQ(geometry.markerCenter.x, 136.f);
        EXPECT_FLOAT_EQ(geometry.hitRectMax.x, 148.f);
        EXPECT_FLOAT_EQ(geometry.menuButtonPosition.x, 100.f);
        EXPECT_LE(geometry.menuButtonPosition.x + 20.f, geometry.hitRectMin.x);
    }

    TEST(PinHitboxGeometryTest, UiScale_ExpandsHitTargetAndKeepsPivotCentered)
    {
        ImVec2 const cellCursor{10.f, 20.f};
        auto const geometry = makePinHitboxGeometry(cellCursor, 72.f, 30.f, 1.5f, true);

        EXPECT_FLOAT_EQ(pinHitTargetSize(1.5f), 36.f);
        EXPECT_FLOAT_EQ(pinMinimumRowHeight(1.5f), 38.f);
        EXPECT_FLOAT_EQ(pinRailWidth(30.f, 7.5f, 1.5f), 87.f);
        EXPECT_FLOAT_EQ(geometry.hitRectMax.x - geometry.hitRectMin.x, 36.f);
        EXPECT_FLOAT_EQ(geometry.markerRadius, 6.f);
        EXPECT_FLOAT_EQ(geometry.pivotRectMin.x + geometry.pivotRectMax.x,
                        2.f * geometry.markerCenter.x);
        EXPECT_FLOAT_EQ(geometry.pivotRectMin.y + geometry.pivotRectMax.y,
                        2.f * geometry.markerCenter.y);
    }

} // namespace gladius::ui::tests
