#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "../../src/ui/NodeView.h"
#include "../../src/ui/LinkDragState.h"
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

    TEST(SharedPinVisualDecision, WithoutActiveDrag_RemainsNormalAndShowsRegularTooltip)
    {
        auto const decision = resolveSharedPinVisualDecision(nullptr,
                                                             nodes::PortId{7},
                                                             true,
                                                             std::type_index{typeid(float)});

        EXPECT_EQ(decision.visualState, PinVisualState::Normal);
        EXPECT_FALSE(decision.showCompatibilityTooltip);
        EXPECT_TRUE(decision.showRegularTooltip);
    }

    TEST(SharedPinVisualDecision, MatchingDirectionAndType_HighlightsPin)
    {
        LinkDragState dragState;
        dragState.beginDrag(nodes::PortId{11}, std::type_index{typeid(float)}, true);

        auto const decision = resolveSharedPinVisualDecision(&dragState,
                                                             nodes::PortId{12},
                                                             true,
                                                             std::type_index{typeid(float)});

        EXPECT_EQ(decision.visualState, PinVisualState::Highlighted);
        EXPECT_TRUE(decision.showCompatibilityTooltip);
        EXPECT_FALSE(decision.showRegularTooltip);
    }

    TEST(SharedPinVisualDecision, IncompatibleDirection_DimsPin)
    {
        LinkDragState dragState;
        dragState.beginDrag(nodes::PortId{11}, std::type_index{typeid(float)}, true);

        auto const decision = resolveSharedPinVisualDecision(&dragState,
                                                             nodes::PortId{12},
                                                             false,
                                                             std::type_index{typeid(float)});

        EXPECT_EQ(decision.visualState, PinVisualState::Dimmed);
        EXPECT_TRUE(decision.showCompatibilityTooltip);
        EXPECT_FALSE(decision.showRegularTooltip);
    }

    TEST(SharedPinVisualDecision, ExplicitCompatibilitySet_OverridesTypeFallback)
    {
        LinkDragState dragState;
        dragState.beginDrag(nodes::PortId{11}, std::type_index{typeid(float)}, true);
        dragState.setCompatiblePorts({42});

        auto const compatibleDecision = resolveSharedPinVisualDecision(&dragState,
                                                                       nodes::PortId{42},
                                                                       true,
                                                                       std::type_index{typeid(nodes::float3)});
        auto const incompatibleDecision = resolveSharedPinVisualDecision(&dragState,
                                                                         nodes::PortId{77},
                                                                         true,
                                                                         std::type_index{typeid(float)});

        EXPECT_EQ(compatibleDecision.visualState, PinVisualState::Highlighted);
        EXPECT_EQ(incompatibleDecision.visualState, PinVisualState::Dimmed);
    }

    TEST(CompactNodeLayoutMetrics, ZeroPorts_StillProducesSingleRowEnvelope)
    {
        auto const metrics = computeCompactNodeLayoutMetrics(0,
                                                             0,
                                                             ImVec2{24.f, 20.f},
                                                             0.f,
                                                             0.f,
                                                             ImVec2{14.f, 14.f},
                                                             4.f,
                                                             2.f);

        EXPECT_EQ(metrics.rowCount, 1);
        EXPECT_GT(metrics.diameter, 0.f);
        EXPECT_GE(metrics.labelColumnWidth, 24.f);
    }

    TEST(CompactNodeLayoutMetrics, MorePins_ExpandsCompactEnvelope)
    {
        auto const oneRow = computeCompactNodeLayoutMetrics(1,
                                                            1,
                                                            ImVec2{24.f, 20.f},
                                                            18.f,
                                                            18.f,
                                                            ImVec2{14.f, 14.f},
                                                            4.f,
                                                            2.f);
        auto const threeRows = computeCompactNodeLayoutMetrics(3,
                                                               2,
                                                               ImVec2{24.f, 20.f},
                                                               18.f,
                                                               18.f,
                                                               ImVec2{14.f, 14.f},
                                                               4.f,
                                                               2.f);

        EXPECT_EQ(threeRows.rowCount, 3);
        EXPECT_GT(threeRows.totalHeight, oneRow.totalHeight);
    }

    TEST(CompactNodeLayoutMetrics, LongLabel_ReservesReadableCenterColumn)
    {
        auto const metrics = computeCompactNodeLayoutMetrics(1,
                                                             1,
                                                             ImVec2{96.f, 20.f},
                                                             24.f,
                                                             24.f,
                                                             ImVec2{14.f, 14.f},
                                                             4.f,
                                                             2.f);

        EXPECT_GE(metrics.labelColumnWidth, 96.f);
        EXPECT_GT(metrics.totalWidth, metrics.labelColumnWidth);
    }

    TEST(CompactNodeLayoutMetrics, LongPortLabels_RequestFallbackRatherThanShrinkingRails)
    {
        auto const metrics = computeCompactNodeLayoutMetrics(2,
                                                             1,
                                                             ImVec2{24.f, 20.f},
                                                             160.f,
                                                             72.f,
                                                             ImVec2{14.f, 14.f},
                                                             4.f,
                                                             2.f);

        EXPECT_TRUE(metrics.useFallbackLayout);
        EXPECT_GE(metrics.railWidth,
                  sharedNodeMetrics().pinMetrics.minimumHitExtent);
    }

    TEST(CompactNodeLayoutMetrics, VisibleSideLabelsStayCompactWhenContentFits)
    {
        auto const metrics = computeCompactNodeLayoutMetrics(2,
                                                             1,
                                                             ImVec2{24.f, 20.f},
                                                             42.f,
                                                             36.f,
                                                             ImVec2{14.f, 14.f},
                                                             4.f,
                                                             2.f);

        EXPECT_FALSE(metrics.useFallbackLayout);
        EXPECT_GT(metrics.inputLabelWidth, 0.f);
        EXPECT_GT(metrics.outputLabelWidth, 0.f);
    }

} // namespace gladius::ui::tests
