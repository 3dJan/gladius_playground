#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/TodoStore.h"

#include <gtest/gtest.h>

namespace gladius::design::tests
{
    TEST(DesignGraph, FreezeAndHybridize_WithFrozenNode_PreservesFrozenVariant)
    {
        // Arrange
        DesignGraph graph;
        auto const standId = graph.addNode(DesignNodeType::Component, "Stand");
        auto const shadeId = graph.addNode(DesignNodeType::Component, "Shade");

        auto const standA = graph.createVariant(standId, "Tripod", "Stable", FidelityLevel::Envelope);
        auto const standB = graph.createVariant(standId, "Pedestal", "Compact", FidelityLevel::Envelope);
        auto const shadeA = graph.createVariant(shadeId, "Cone", "Simple", FidelityLevel::Envelope);
        auto const shadeB = graph.createVariant(shadeId, "Ribbed", "Diffused", FidelityLevel::Envelope);

        auto const cfgA = graph.createConfiguration("Concept A", {{standId, standA}, {shadeId, shadeA}});
        auto const cfgB = graph.createConfiguration("Concept B", {{standId, standB}, {shadeId, shadeB}});

        ASSERT_TRUE(graph.activateConfiguration(cfgA));
        ASSERT_TRUE(graph.freezeNode(standId));

        // Act: hybridize Concept A + Concept B attempting to override both, standId is frozen
        auto const hybridId = graph.hybridizeConfigurations("Hybrid", cfgA, cfgB, {standId, shadeId});
        ASSERT_TRUE(hybridId.has_value());
        ASSERT_TRUE(graph.activateConfiguration(*hybridId));

        // Assert
        auto const active = graph.activeConfiguration();
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->selectedVariants.at(standId), standA);
        EXPECT_EQ(active->selectedVariants.at(shadeId), shadeB);
    }

    TEST(DesignCatalogue, LoadAndPruneBeam_IncompatibleCombination_FiltersAndRanks)
    {
        // Arrange
        DesignCatalogue catalogue;
        nlohmann::json doc = {
            {"entries", {{
                {"id", "lamp.shade.ribbed"},
                {"functions", {"diffuse_light"}},
                {"parameters", {{"wallThickness", {{"type", "length"}, {"minimum", 1.2}, {"maximum", 4.0}}}}},
                {"manufacturing", {{"processes", {"FDM"}}, {"supportsRequired", false}}},
                {"interfaces", {"stand_mount"}}
            }}}
        };
        ASSERT_TRUE(catalogue.loadFromJson(doc));
        catalogue.addCompatibilityRule({{{"shade", "heavy_glass"}, {"stand", "thin_tripod"}}, false, "Too top-heavy"});

        std::vector<std::string> paramErrors;
        EXPECT_TRUE(catalogue.validateParameters("lamp.shade.ribbed", {{"wallThickness", 1.6}}, paramErrors));
        EXPECT_FALSE(catalogue.validateParameters("lamp.shade.ribbed", {{"wallThickness", 0.5}}, paramErrors));

        Configuration c1{"c1", "Bad", {{"shade", "heavy_glass"}, {"stand", "thin_tripod"}}, Evaluation{{}, {}, {}, {}, 10.0}, false};
        Configuration c2{"c2", "Good Low", {{"shade", "ribbed"}, {"stand", "pedestal"}}, Evaluation{{}, {}, {}, {}, 5.0}, false};
        Configuration c3{"c3", "Good High", {{"shade", "cone"}, {"stand", "pedestal"}}, Evaluation{{}, {}, {}, {}, 8.0}, false};

        // Act
        auto const beam = catalogue.pruneBeam({c1, c2, c3}, 2);

        // Assert
        ASSERT_EQ(beam.size(), 2u);
        EXPECT_EQ(beam[0].id, "c3");
        EXPECT_EQ(beam[1].id, "c2");
    }

    TEST(DesignGraph, UpdateAndRemoveRequirement_TracksPendingChangesAndClearsOnConsume)
    {
        // Arrange
        DesignGraph graph;
        auto const r1 = graph.addRequirement("Height >= 50mm", "dimensions", true);
        auto const r2 = graph.addRequirement("PLA printable", "manufacturing", true);

        // Act
        ASSERT_TRUE(graph.updateRequirement(r1, "Height >= 60mm", "dimensions", true, true));
        ASSERT_TRUE(graph.removeRequirement(r2, true));
        EXPECT_TRUE(graph.hasPendingRequirementChanges());
        auto const changes = graph.consumePendingRequirementChanges();

        // Assert
        ASSERT_EQ(changes.size(), 2u);
        EXPECT_FALSE(graph.hasPendingRequirementChanges());
        ASSERT_EQ(graph.requirements().size(), 1u);
        EXPECT_EQ(graph.requirements()[0].description, "Height >= 60mm");
    }

    TEST(DesignGraph, ReopenForFollowUpRequest_CompletedDesign_ResetsPhaseAndRecordsChange)
    {
        // Arrange
        DesignGraph graph;
        graph.addRequirement("Height >= 50mm", "dimensions", true);
        graph.setPhase(DesignPhase::Complete);

        // Act
        ASSERT_TRUE(graph.reopenForFollowUpRequest("Build a house out of the brick I added"));

        // Assert
        EXPECT_EQ(graph.phase(), DesignPhase::Refine);
        EXPECT_TRUE(graph.hasPendingRequirementChanges());
        auto const changes = graph.consumePendingRequirementChanges();
        ASSERT_EQ(changes.size(), 1u);
        EXPECT_NE(changes[0].find("Build a house out of the brick I added"), std::string::npos);
        EXPECT_EQ(graph.requirements().size(), 1u);
    }

    TEST(DesignGraph, ReopenForFollowUpRequest_IncompleteDesign_DoesNothing)
    {
        // Arrange
        DesignGraph graph;
        graph.setPhase(DesignPhase::DevelopComponents);

        // Act / Assert
        EXPECT_FALSE(graph.reopenForFollowUpRequest("Make it taller"));
        EXPECT_EQ(graph.phase(), DesignPhase::DevelopComponents);
        EXPECT_FALSE(graph.hasPendingRequirementChanges());
    }

    TEST(DesignGraph, ToAndFromJson_RoundTripsFeedbackAndConfigurations)
    {
        // Arrange
        DesignGraph graph;
        auto const n1 = graph.addNode(DesignNodeType::Component, "Base");
        auto const v1 = graph.createVariant(n1, "Hex", "Strong", FidelityLevel::Structural);
        ASSERT_TRUE(graph.setVariantFeedback(v1, "Looks sturdy"));
        auto const c1 = graph.createConfiguration("Config 1", {{n1, v1}});
        ASSERT_TRUE(graph.setConfigurationFeedback(c1, "Prefer thinner walls"));
        ASSERT_TRUE(graph.activateConfiguration(c1));

        // Act
        nlohmann::json const serialized = graph.toJson();
        DesignGraph restored;
        ASSERT_TRUE(restored.fromJson(serialized));

        // Assert
        ASSERT_EQ(restored.variants().size(), 1u);
        EXPECT_EQ(restored.variants().at(v1).userFeedback, "Looks sturdy");
        ASSERT_TRUE(restored.activeConfiguration().has_value());
        EXPECT_EQ(restored.activeConfiguration()->userFeedback, "Prefer thinner walls");
    }

    TEST(TodoStore, AddUpdateAndJson_RoundTripsChecklistState)
    {
        // Arrange
        TodoStore store;
        auto const id = store.addItem(
          "Build base",
          "Create mounting flange",
          {ChecklistEntry{"Sketch", false}, ChecklistEntry{"Validate", false}});

        // Act
        ASSERT_TRUE(store.updateItem(id, TodoStatus::InProgress));
        ASSERT_TRUE(store.toggleChecklistEntry(id, 0, true));
        nlohmann::json const j = store.toJson();
        TodoStore restored;
        ASSERT_TRUE(restored.fromJson(j));

        // Assert
        ASSERT_EQ(restored.items().size(), 1u);
        EXPECT_EQ(restored.items()[0].status, TodoStatus::InProgress);
        EXPECT_TRUE(restored.items()[0].checklist[0].checked);
        EXPECT_FALSE(restored.items()[0].checklist[1].checked);
    }
} // namespace gladius::design::tests
