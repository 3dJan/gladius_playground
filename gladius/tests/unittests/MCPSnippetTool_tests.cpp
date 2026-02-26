/**
 * @file MCPSnippetTool_tests.cpp
 * @brief Unit tests for the MCP snippet tool logic (get/set function snippet).
 *
 * These tests exercise the converter-level logic that the MCP tools rely on,
 * without requiring a full Application instance.
 */

#include <gtest/gtest.h>

#include "ExpressionParser.h"
#include "ExpressionToGraphConverter.h"
#include "FunctionArgument.h"
#include "nodes/Assembly.h"
#include "nodes/Model.h"

namespace gladius::tests
{
    class MCPSnippetToolTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_model = std::make_unique<nodes::Model>();
            m_parser = std::make_unique<ExpressionParser>();
        }

        /// Simulate get_function_snippet: convert graph to snippet
        std::string getFunctionSnippet()
        {
            auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
              *m_model, m_args, m_output);
            return snippet.empty() ? "return 0;" : snippet;
        }

        /// Simulate set_function_snippet: parse, validate, replace, normalize
        std::pair<bool, std::string> setFunctionSnippet(std::string const & snippet)
        {
            // Reject unsupported comments
            if (snippet.find(ExpressionToGraphConverter::UNSUPPORTED_NODE_MARKER) != std::string::npos)
            {
                return {false, "Snippet contains unsupported node placeholders"};
            }

            // Validate by parsing into temp model
            nodes::Model tempModel;
            tempModel.createBeginEndWithDefaultInAndOuts();
            auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, tempModel, *m_parser, m_args, m_output);
            if (nodeId == 0)
            {
                return {false, "Failed to parse snippet"};
            }

            // Apply to real model
            m_model->clear();
            m_model->createBeginEndWithDefaultInAndOuts();
            ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, *m_model, *m_parser, m_args, m_output);
            m_model->updateGraphAndOrderIfNeeded();

            // Return normalized
            auto normalized = ExpressionToGraphConverter::convertGraphToSnippet(
              *m_model, m_args, m_output);
            return {true, normalized.empty() ? snippet : normalized};
        }

        std::unique_ptr<nodes::Model> m_model;
        std::unique_ptr<ExpressionParser> m_parser;
        std::vector<FunctionArgument> m_args = {{"pos", ArgumentType::Vector}};
        FunctionOutput m_output = FunctionOutput::defaultOutput();
    };

    // T049: get_function_snippet returns valid snippet for existing function

    TEST_F(MCPSnippetToolTest, GetSnippet_ExistingFunction_ReturnsValidSnippet)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x + pos.y;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        auto snippet = getFunctionSnippet();
        EXPECT_FALSE(snippet.empty());
        EXPECT_NE(snippet.find("return"), std::string::npos);
    }

    // T050: get_function_snippet returns fallback for empty graph

    TEST_F(MCPSnippetToolTest, GetSnippet_EmptyGraph_ReturnsFallback)
    {
        // No graph setup — model has no nodes
        auto snippet = getFunctionSnippet();
        EXPECT_EQ(snippet, "return 0;");
    }

    // T051: set_function_snippet replaces graph and returns normalized snippet

    TEST_F(MCPSnippetToolTest, SetSnippet_ValidCode_ReplacesGraphAndNormalizes)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        auto [success, normalized] = setFunctionSnippet("return pos.x + pos.y * 2;");
        EXPECT_TRUE(success);
        EXPECT_FALSE(normalized.empty());
        EXPECT_NE(normalized.find("return"), std::string::npos);

        // Verify idempotency: set again with normalized output should produce same
        auto [success2, normalized2] = setFunctionSnippet(normalized);
        EXPECT_TRUE(success2);
        EXPECT_EQ(normalized, normalized2);
    }

    // T052: set_function_snippet with parse error returns error and preserves graph

    TEST_F(MCPSnippetToolTest, SetSnippet_ParseError_PreservesGraphAndReturnsError)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();
        auto originalSnippet = getFunctionSnippet();

        auto [success, errorMsg] = setFunctionSnippet("return +++invalid garbage;");
        EXPECT_FALSE(success);
        EXPECT_FALSE(errorMsg.empty());

        // Graph should be unchanged
        auto currentSnippet = getFunctionSnippet();
        EXPECT_EQ(originalSnippet, currentSnippet);
    }

    TEST_F(MCPSnippetToolTest, SetSnippet_UnsupportedComment_Rejected)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        auto [success, msg] =
          setFunctionSnippet("return /* unsupported: SomeNode */ + pos.x;");
        EXPECT_FALSE(success);
        EXPECT_NE(msg.find("unsupported"), std::string::npos);
    }

    // =====================================================================================
    // T061: get_program_snippet returns all functions in order
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, GetProgramSnippet_MultipleFunctions_ReturnsAllInOrder)
    {
        nodes::Assembly assembly;
        assembly.addModelIfNotExisting(10);
        assembly.addModelIfNotExisting(20);

        auto model10 = assembly.findModel(10);
        model10->setDisplayName("base");
        model10->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model10, *m_parser, m_args, m_output);
        model10->updateGraphAndOrderIfNeeded();

        auto model20 = assembly.findModel(20);
        model20->setDisplayName("derived");
        model20->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.y;", *model20, *m_parser, m_args, m_output);
        model20->updateGraphAndOrderIfNeeded();

        auto result = ExpressionToGraphConverter::convertProgramToSnippet(assembly);
        EXPECT_FALSE(result.empty());
        EXPECT_NE(result.find("base_10"), std::string::npos);
        EXPECT_NE(result.find("derived_20"), std::string::npos);
        EXPECT_NE(result.find("return"), std::string::npos);
    }

    // =====================================================================================
    // T066: set_program_snippet creates all functions with correct cross-references
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetProgramSnippet_MultipleFunctions_CreatesAll)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        std::string program =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.x;\n"
          "}\n"
          "\n"
          "// Function: combo (ID: 20)\n"
          "float combo_20(vec3 pos) {\n"
          "  return pos.y;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program, assembly, parser);

        auto model10 = assembly.findModel(10);
        ASSERT_NE(model10, nullptr) << "Function ID 10 should exist";
        EXPECT_EQ(model10->getDisplayName().value_or(""), "sphere");

        auto model20 = assembly.findModel(20);
        ASSERT_NE(model20, nullptr) << "Function ID 20 should exist";
        EXPECT_EQ(model20->getDisplayName().value_or(""), "combo");

        // Both functions should produce valid snippets
        auto snippet10 = ExpressionToGraphConverter::convertGraphToSnippet(
          *model10, m_args, m_output, &assembly);
        EXPECT_NE(snippet10.find("return"), std::string::npos);

        auto snippet20 = ExpressionToGraphConverter::convertGraphToSnippet(
          *model20, m_args, m_output, &assembly);
        EXPECT_NE(snippet20.find("return"), std::string::npos);
    }

    // =====================================================================================
    // T067: removing a function that still has callers is rejected
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetProgramSnippet_MissingCalledFunction_Throws)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        // combo calls sphere_10 but sphere is not defined
        std::string program =
          "// Function: combo (ID: 20)\n"
          "float combo_20(vec3 pos) {\n"
          "  return sphere_10(pos);\n"
          "}\n";

        EXPECT_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, assembly, parser),
          std::runtime_error);
    }

} // namespace gladius::tests
