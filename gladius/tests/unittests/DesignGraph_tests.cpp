#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"

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
} // namespace gladius::design::tests
