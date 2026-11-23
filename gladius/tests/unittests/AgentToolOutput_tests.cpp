#include "design/AgentToolOutput.h"

#include <gtest/gtest.h>
#include <string>

namespace gladius::design::tests
{
    namespace
    {
        constexpr char const * kProgram =
          "// Function: main (ID: 3) [root]\n"
          "(float shape) main_3(vec3 pos) {\n"
          "  shape = helper_sphere_9(pos);\n"
          "}\n"
          "// Function: helper_sphere (ID: 9)\n"
          "float helper_sphere_9(vec3 pos) {\n"
          "  return sqrt(pos.x * pos.x) - 5.0;\n"
          "}\n";
    } // namespace

    TEST(compactDesignGraph, WithVariantPreview_PreviewAndCountersRemoved)
    {
        // Arrange
        nlohmann::json graph = {
            {"phase", "DesignModel"},
            {"requirements", nlohmann::json::array({{{"id", "req_1"}, {"description", "fits"}}})},
            {"nodes", nlohmann::json::array({{{"id", "n1"}, {"name", "root"}}})},
            {"variants",
             nlohmann::json::array(
               {{{"id", "var_1"},
                 {"name", "gyroid"},
                 {"previewPngBase64", std::string(4000, 'A')},
                 {"userFeedback", ""},
                 {"parameters", nlohmann::json::object()},
                 {"evaluation", {{"deterministicPassed", true},
                                 {"deterministicWarnings", nlohmann::json::array()},
                                 {"heuristicNotes", nlohmann::json::array()},
                                 {"subjectiveNotes", nlohmann::json::array()},
                                 {"pruningScore", 0.0}}}}})},
            {"nextReqId", 4},
            {"nextNodeId", 7},
            {"nextVariantId", 2},
            {"nextConfigId", 1},
            {"success", true}};

        // Act
        auto const compacted = gladius::design::agent_tool_output::compactDesignGraph(graph);

        // Assert
        auto const & variant = compacted["variants"][0];
        EXPECT_FALSE(variant.contains("previewPngBase64"));
        EXPECT_FALSE(variant.contains("evaluation"));
        EXPECT_FALSE(variant.contains("userFeedback"));
        EXPECT_FALSE(variant.contains("parameters"));
        EXPECT_EQ(compacted["success"], true);
        EXPECT_EQ(compacted["phase"], "DesignModel");
        for (auto key : {"nextReqId", "nextNodeId", "nextVariantId", "nextConfigId"})
        {
            EXPECT_FALSE(compacted.contains(key));
        }
    }

    TEST(compactDesignGraph, WithWarnings_EvaluationIsKept)
    {
        // Arrange
        nlohmann::json graph = {
            {"variants",
             nlohmann::json::array(
               {{{"id", "var_1"},
                 {"evaluation", {{"deterministicWarnings", nlohmann::json::array({"too thin"})},
                                 {"pruningScore", 0.0}}}}})}};

        // Act
        auto const compacted = gladius::design::agent_tool_output::compactDesignGraph(graph);

        // Assert: the model has to be able to see and fix the problem
        EXPECT_EQ(compacted["variants"][0]["evaluation"]["deterministicWarnings"].size(), 1u);
    }

    TEST(compactDesignGraph, OversizedCollections_AreCappedAndMarked)
    {
        // Arrange
        nlohmann::json nodes = nlohmann::json::array();
        for (int i = 0; i < 200; ++i)
        {
            nodes.push_back({{"id", "node_" + std::to_string(i)},
                             {"name", std::string(40, 'n') + std::to_string(i)}});
        }
        nlohmann::json graph = {{"nodes", nodes}};

        // Act
        auto const compacted = gladius::design::agent_tool_output::compactDesignGraph(graph);

        // Assert: valid JSON, strictly fewer entries, and the model is told about the cut
        ASSERT_TRUE(compacted["nodes"].is_array());
        EXPECT_LT(compacted["nodes"].size(), nodes.size());
        EXPECT_TRUE(compacted["nodes"].back().contains("trimmed"));
        EXPECT_LT(compacted["nodes"].dump().size(), 5000u);
    }

    TEST(capProgramListing, ListingFits_ReturnedUnchanged)
    {
        EXPECT_EQ(gladius::design::agent_tool_output::capProgramListing(kProgram, 4096), kProgram);
    }

    TEST(capProgramListing, OversizedListing_CutOnFunctionBoundary)
    {
        // Act: budget only fits one of the two functions
        auto const capped = gladius::design::agent_tool_output::capProgramListing(kProgram, 90);

        // Assert
        EXPECT_NE(capped.find("main_3"), std::string::npos);
        EXPECT_EQ(capped.find("helper_sphere_9"), std::string::npos);
        EXPECT_NE(capped.find("trimmed"), std::string::npos);
        EXPECT_LE(capped.size(), std::string_view(kProgram).size());
    }

    TEST(filterProgramListing, NamedFunction_OnlyThatBlockReturned)
    {
        // Act
        auto const filtered =
          gladius::design::agent_tool_output::filterProgramListing(kProgram, {"helper_sphere"});

        // Assert
        EXPECT_NE(filtered.find("helper_sphere_9"), std::string::npos);
        EXPECT_EQ(filtered.find("main_3"), std::string::npos);
    }

    TEST(filterProgramListing, NoMatch_EmptyResult)
    {
        EXPECT_TRUE(gladius::design::agent_tool_output::filterProgramListing(kProgram, {"nope"})
                      .empty());
    }
} // namespace gladius::design::tests
