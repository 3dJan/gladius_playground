#include "design/DesignGraph.h"
#include "design/RequirementEditQueue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gladius::design::tests
{
    namespace
    {
        [[nodiscard]] bool hasRequirement(DesignGraph const & graph, std::string const & description)
        {
            return std::any_of(graph.requirements().begin(),
                               graph.requirements().end(),
                               [&](Requirement const & r) { return r.description == description; });
        }
    } // namespace

    TEST(RequirementEditQueue, MergedView_WithStagedEdits_ShowsThemWithoutTouchingTheGraph)
    {
        // Arrange
        DesignGraph graph;
        std::string const id = graph.addRequirement("Hold 5kg", "functional", true);
        RequirementEditQueue edits;

        // Act
        edits.stageUpdate(id, "Hold 12kg", "functional", true);
        edits.stageAdd("Must be walnut", "aesthetic", false);
        auto const view = edits.mergedView(graph);

        // Assert: the user sees the edit immediately, the run thread's graph stays untouched
        ASSERT_EQ(view.size(), 2u);
        auto const it = std::find_if(view.begin(), view.end(), [&](Requirement const & r) {
            return r.id == id;
        });
        ASSERT_NE(it, view.end());
        EXPECT_EQ(it->description, "Hold 12kg");
        EXPECT_TRUE(hasRequirement(graph, "Hold 5kg"));
        EXPECT_FALSE(hasRequirement(graph, "Must be walnut"));
    }

    TEST(RequirementEditQueue, ApplyTo_WithUpdateAddRemove_WritesGraphAndRecordsChanges)
    {
        // Arrange
        DesignGraph graph;
        std::string const keepId = graph.addRequirement("Span 800mm", "functional", true);
        std::string const dropId = graph.addRequirement("Fold flat", "functional", false);
        RequirementEditQueue edits;
        edits.stageUpdate(keepId, "Span 900mm", "functional", true);
        edits.stageRemove(dropId);
        edits.stageAdd("Wall mounted", "installation", true);

        // Act
        edits.applyTo(graph);

        // Assert
        EXPECT_FALSE(hasRequirement(graph, "Fold flat"));
        EXPECT_TRUE(hasRequirement(graph, "Span 900mm"));
        EXPECT_TRUE(hasRequirement(graph, "Wall mounted"));
        auto const changes = graph.consumePendingRequirementChanges();
        auto const hasLine = [&](std::string const & needle) {
            return std::any_of(changes.begin(), changes.end(), [&](std::string const & line) {
                return line.find(needle) != std::string::npos;
            });
        };
        EXPECT_TRUE(hasLine("Updated requirement " + keepId));
        EXPECT_TRUE(hasLine("Removed requirement " + dropId));
        EXPECT_TRUE(hasLine("Added requirement req_"));
        EXPECT_TRUE(edits.empty());
    }

    TEST(RequirementEditQueue, ApplyTo_EditOfStagedAddition_AppliesToTheRealGraphId)
    {
        // Arrange: the user adds a requirement mid-run and edits that row before the run applies it
        DesignGraph graph;
        RequirementEditQueue edits;
        edits.stageAdd("Silent hinges", "functional", true);
        std::string const stagedId = edits.mergedView(graph).front().id;
        edits.stageUpdate(stagedId, "Soft-close hinges", "functional", false);

        // Act
        edits.applyTo(graph);

        // Assert: one requirement, renamed, and it is still tracked as a user change
        ASSERT_EQ(graph.requirements().size(), 1u);
        EXPECT_EQ(graph.requirements().front().description, "Soft-close hinges");
        EXPECT_FALSE(graph.requirements().front().mandatory);
        auto const changes = graph.consumePendingRequirementChanges();
        EXPECT_EQ(changes.size(), 1u);
        EXPECT_NE(changes.front().find("Added requirement req_"), std::string::npos);
    }

    TEST(RequirementEditQueue, RepeatedEditsOfOneRequirement_CollapseIntoOneChangeLine)
    {
        // Arrange: keystrokes on one row must not each become a change line for the model
        DesignGraph graph;
        std::string const id = graph.addRequirement("Hold 5kg", "functional", true);
        RequirementEditQueue edits;
        edits.stageUpdate(id, "Hold 6kg", "functional", true);
        edits.stageUpdate(id, "Hold 7kg", "functional", true);
        edits.stageUpdate(id, "Hold 8kg", "functional", true);

        // Act
        edits.applyTo(graph);

        // Assert
        EXPECT_EQ(edits.size(), 0u);
        EXPECT_TRUE(hasRequirement(graph, "Hold 8kg"));
        auto const changes = graph.consumePendingRequirementChanges();
        EXPECT_EQ(changes.size(), 1u);
    }

    TEST(RequirementEditQueue, Clear_WithStagedEdits_LeavesGraphUnchanged)
    {
        // Arrange
        DesignGraph graph;
        graph.addRequirement("Keep me", "functional", true);
        RequirementEditQueue edits;
        edits.stageAdd("Drop me", "functional", true);

        // Act
        edits.clear();
        edits.applyTo(graph);

        // Assert
        EXPECT_FALSE(hasRequirement(graph, "Drop me"));
        EXPECT_FALSE(graph.hasPendingRequirementChanges());
    }
} // namespace gladius::design::tests
