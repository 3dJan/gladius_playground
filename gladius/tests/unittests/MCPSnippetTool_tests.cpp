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

    // =====================================================================================
    // T003: isReservedKeyword accepts valid names and rejects reserved keywords
    // =====================================================================================

    TEST(ArgumentUtilsTest, IsReservedKeyword_ReservedTypes_ReturnsTrue)
    {
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("float"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("vec3"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("int"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("void"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("bool"));
    }

    TEST(ArgumentUtilsTest, IsReservedKeyword_ControlFlow_ReturnsTrue)
    {
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("return"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("if"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("else"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("for"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("while"));
    }

    TEST(ArgumentUtilsTest, IsReservedKeyword_BuiltinFunctions_ReturnsTrue)
    {
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("sin"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("length"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("normalize"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("smoothstep"));
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("clamp"));
    }

    TEST(ArgumentUtilsTest, IsReservedKeyword_ValidNames_ReturnsFalse)
    {
        EXPECT_FALSE(ArgumentUtils::isReservedKeyword("pos"));
        EXPECT_FALSE(ArgumentUtils::isReservedKeyword("radius"));
        EXPECT_FALSE(ArgumentUtils::isReservedKeyword("height"));
        EXPECT_FALSE(ArgumentUtils::isReservedKeyword("myVar"));
        EXPECT_FALSE(ArgumentUtils::isReservedKeyword("scale_factor"));
    }

    // =====================================================================================
    // T004: get_function_snippet returns arguments for multi-argument function
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, GetSnippet_MultiArgFunction_ReturnsArguments)
    {
        m_args = {{"pos", ArgumentType::Vector}, {"radius", ArgumentType::Scalar}};
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return length(pos) - radius;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          *m_model, m_args, m_output);
        EXPECT_FALSE(snippet.empty());

        // Verify Begin node has both argument ports
        auto * beginNode = m_model->getBeginNode();
        ASSERT_NE(beginNode, nullptr);
        auto const & outputs = beginNode->getOutputs();
        EXPECT_EQ(outputs.size(), 2u);
        EXPECT_NE(outputs.find("pos"), outputs.end());
        EXPECT_NE(outputs.find("radius"), outputs.end());
    }

    // =====================================================================================
    // T005: get_function_snippet returns correct output_type
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, GetSnippet_ScalarOutput_ReturnsFloat)
    {
        m_output = FunctionOutput("result", ArgumentType::Scalar);
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        // End node should have scalar connection
        auto * endNode = m_model->getEndNode();
        ASSERT_NE(endNode, nullptr);
        auto const & params = endNode->constParameter();
        auto shapeIt = params.find(nodes::FieldNames::Shape);
        EXPECT_NE(shapeIt, params.end());
    }

    // =====================================================================================
    // T006: get_function_snippet returns empty arguments for no-argument function
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, GetSnippet_NoArgs_NoExtraArguments)
    {
        // With no explicit args, convertSnippetToGraph creates no extra Begin outputs
        // beyond those the model defaults to. A "return 5.0;" body needs no inputs.
        std::vector<FunctionArgument> noArgs;
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return 5.0;", *m_model, *m_parser, noArgs, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        // The snippet "return 5.0" should produce a valid snippet without requiring args
        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          *m_model, noArgs, m_output);
        EXPECT_NE(snippet.find("return"), std::string::npos);
    }

    // =====================================================================================
    // T007: Signature round-trip fidelity
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SignatureRoundTrip_GetSetGet_IdenticalArguments)
    {
        m_args = {{"pos", ArgumentType::Vector}, {"radius", ArgumentType::Scalar}};
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return length(pos) - radius;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        // Get first snippet
        auto snippet1 = ExpressionToGraphConverter::convertGraphToSnippet(
          *m_model, m_args, m_output);
        EXPECT_FALSE(snippet1.empty());

        // Set it back
        nodes::Model tempModel;
        tempModel.createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          snippet1, tempModel, *m_parser, m_args, m_output);
        tempModel.updateGraphAndOrderIfNeeded();

        // Get second snippet
        auto snippet2 = ExpressionToGraphConverter::convertGraphToSnippet(
          tempModel, m_args, m_output);
        EXPECT_EQ(snippet1, snippet2);

        // Both models should have same Begin node outputs
        auto * begin1 = m_model->getBeginNode();
        auto * begin2 = tempModel.getBeginNode();
        ASSERT_NE(begin1, nullptr);
        ASSERT_NE(begin2, nullptr);
        EXPECT_EQ(begin1->getOutputs().size(), begin2->getOutputs().size());
    }

    // =====================================================================================
    // T008: set_function_snippet creates correct argument ports on Begin node
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetSnippet_MultiArgs_CreatesBeginNodePorts)
    {
        m_args = {{"pos", ArgumentType::Vector}, {"radius", ArgumentType::Scalar}};
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return length(pos) - radius;", *m_model, *m_parser, m_args, m_output);
        EXPECT_NE(nodeId, 0u);
        m_model->updateGraphAndOrderIfNeeded();

        auto * beginNode = m_model->getBeginNode();
        ASSERT_NE(beginNode, nullptr);
        auto const & outputs = beginNode->getOutputs();
        EXPECT_EQ(outputs.size(), 2u);

        auto posIt = outputs.find("pos");
        ASSERT_NE(posIt, outputs.end());
        EXPECT_EQ(posIt->second.getTypeIndex(), nodes::ParameterTypeIndex::Float3);

        auto radiusIt = outputs.find("radius");
        ASSERT_NE(radiusIt, outputs.end());
        EXPECT_EQ(radiusIt->second.getTypeIndex(), nodes::ParameterTypeIndex::Float);
    }

    // =====================================================================================
    // T009: set_function_snippet updates arguments on existing function (add new argument)
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetSnippet_AddArgument_UpdatesBeginNode)
    {
        // Initial setup with one argument
        m_args = {{"pos", ArgumentType::Vector}};
        m_model->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *m_model, *m_parser, m_args, m_output);
        m_model->updateGraphAndOrderIfNeeded();

        // Now set with two arguments
        std::vector<FunctionArgument> newArgs = {
          {"pos", ArgumentType::Vector}, {"scale", ArgumentType::Scalar}};

        nodes::Model updatedModel;
        updatedModel.createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x * scale;", updatedModel, *m_parser, newArgs, m_output);
        EXPECT_NE(nodeId, 0u);
        updatedModel.updateGraphAndOrderIfNeeded();

        auto * beginNode = updatedModel.getBeginNode();
        ASSERT_NE(beginNode, nullptr);
        auto const & outputs = beginNode->getOutputs();
        EXPECT_EQ(outputs.size(), 2u);
        EXPECT_NE(outputs.find("scale"), outputs.end());
    }

    // =====================================================================================
    // T010: create_function_from_snippet with arguments creates correct Begin node ports
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, CreateFromSnippet_WithArgs_CreatesBeginNodePorts)
    {
        std::vector<FunctionArgument> args = {
          {"pos", ArgumentType::Vector}, {"height", ArgumentType::Scalar}};
        FunctionOutput output("result", ArgumentType::Scalar);

        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.y - height;", model, *m_parser, args, output);
        EXPECT_NE(nodeId, 0u);
        model.updateGraphAndOrderIfNeeded();

        auto * beginNode = model.getBeginNode();
        ASSERT_NE(beginNode, nullptr);
        auto const & outputs = beginNode->getOutputs();
        EXPECT_EQ(outputs.size(), 2u);
        EXPECT_NE(outputs.find("pos"), outputs.end());
        EXPECT_NE(outputs.find("height"), outputs.end());
    }

    // =====================================================================================
    // T013: Reserved keyword rejection for set_function_snippet
    // =====================================================================================

    TEST(ArgumentUtilsTest, IsReservedKeyword_FloatAsArgName_Rejected)
    {
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("float"));
    }

    // =====================================================================================
    // T014: Reserved keyword rejection for create_function_from_snippet
    // =====================================================================================

    TEST(ArgumentUtilsTest, IsReservedKeyword_LengthAsArgName_Rejected)
    {
        EXPECT_TRUE(ArgumentUtils::isReservedKeyword("length"));
    }

    // =====================================================================================
    // T020: get_program_snippet returns functions in topological order with full signatures
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, GetProgramSnippet_MultipleWithSignatures_TopologicalOrder)
    {
        nodes::Assembly assembly;
        assembly.addModelIfNotExisting(10);
        assembly.addModelIfNotExisting(20);

        auto model10 = assembly.findModel(10);
        model10->setDisplayName("sphere");
        model10->createBeginEndWithDefaultInAndOuts();
        std::vector<FunctionArgument> sphereArgs = {
          {"pos", ArgumentType::Vector}, {"radius", ArgumentType::Scalar}};
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return length(pos) - radius;", *model10, *m_parser, sphereArgs, m_output);
        model10->updateGraphAndOrderIfNeeded();

        auto model20 = assembly.findModel(20);
        model20->setDisplayName("shell");
        model20->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model20, *m_parser, m_args, m_output);
        model20->updateGraphAndOrderIfNeeded();

        auto result = ExpressionToGraphConverter::convertProgramToSnippet(assembly);
        EXPECT_FALSE(result.empty());

        // Should contain both function signatures
        EXPECT_NE(result.find("sphere_10"), std::string::npos);
        EXPECT_NE(result.find("shell_20"), std::string::npos);

        // sphere_10 should have multi-arg signature
        EXPECT_NE(result.find("vec3 pos"), std::string::npos);
        EXPECT_NE(result.find("float radius"), std::string::npos);
    }

    // =====================================================================================
    // T021: set_program_snippet with modified function arguments updates graph
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetProgramSnippet_ModifiedArguments_UpdatesGraph)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        // First create with one argument
        std::string program1 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.x;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program1, assembly, parser);

        // Now update with two arguments
        std::string program2 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos, float radius) {\n"
          "  float v0 = length(pos);\n"
          "  float v1 = v0 - radius;\n"
          "  return v1;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program2, assembly, parser);

        auto model10 = assembly.findModel(10);
        ASSERT_NE(model10, nullptr);

        auto * beginNode = model10->getBeginNode();
        ASSERT_NE(beginNode, nullptr);
        auto const & outputs = beginNode->getOutputs();
        EXPECT_EQ(outputs.size(), 2u);
        EXPECT_NE(outputs.find("pos"), outputs.end());
        EXPECT_NE(outputs.find("radius"), outputs.end());
    }

    // =====================================================================================
    // T022: set_program_snippet with new function creates new function resource
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetProgramSnippet_NewFunction_CreatesResource)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        // Create initial function
        std::string program1 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.x;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program1, assembly, parser);
        EXPECT_NE(assembly.findModel(10), nullptr);

        // Add a new function
        std::string program2 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.x;\n"
          "}\n"
          "\n"
          "// Function: box (ID: 30)\n"
          "float box_30(vec3 pos) {\n"
          "  return pos.y;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program2, assembly, parser);
        EXPECT_NE(assembly.findModel(10), nullptr);
        EXPECT_NE(assembly.findModel(30), nullptr);
    }

    // =====================================================================================
    // T023: set_program_snippet preserves functions not in snippet
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, SetProgramSnippet_PreservesExistingFunctions)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        // Create two functions
        std::string program1 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.x;\n"
          "}\n"
          "\n"
          "// Function: box (ID: 20)\n"
          "float box_20(vec3 pos) {\n"
          "  return pos.y;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program1, assembly, parser);
        EXPECT_NE(assembly.findModel(10), nullptr);
        EXPECT_NE(assembly.findModel(20), nullptr);

        // Update only sphere — box should still exist
        std::string program2 =
          "// Function: sphere (ID: 10)\n"
          "float sphere_10(vec3 pos) {\n"
          "  return pos.z;\n"
          "}\n";

        ExpressionToGraphConverter::setProgramSnippet(program2, assembly, parser);
        EXPECT_NE(assembly.findModel(10), nullptr);
        EXPECT_NE(assembly.findModel(20), nullptr);
    }

    // =====================================================================================
    // T024: Program round-trip: get → set → get produces equivalent output
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, ProgramRoundTrip_GetSetGet_Equivalent)
    {
        nodes::Assembly assembly;
        ExpressionParser parser;

        assembly.addModelIfNotExisting(10);
        auto model10 = assembly.findModel(10);
        model10->setDisplayName("sphere");
        model10->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model10, *m_parser, m_args, m_output);
        model10->updateGraphAndOrderIfNeeded();

        // Get program
        auto snippet1 = ExpressionToGraphConverter::convertProgramToSnippet(assembly);
        EXPECT_FALSE(snippet1.empty());

        // Set it back
        nodes::Assembly assembly2;
        ExpressionToGraphConverter::setProgramSnippet(snippet1, assembly2, parser);

        // Get again
        auto snippet2 = ExpressionToGraphConverter::convertProgramToSnippet(assembly2);
        EXPECT_FALSE(snippet2.empty());

        // Both should contain the same function
        EXPECT_NE(snippet1.find("sphere_10"), std::string::npos);
        EXPECT_NE(snippet2.find("sphere_10"), std::string::npos);
    }

    // =====================================================================================
    // T017: get_program_snippet response contains root_functions array with correct resource IDs
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, AnnotateRootFunctions_WithRootIds_AddsRootAnnotation)
    {
        nodes::Assembly assembly;
        assembly.addModelIfNotExisting(10);
        assembly.addModelIfNotExisting(20);

        auto model10 = assembly.findModel(10);
        model10->setDisplayName("sphere");
        model10->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return length(pos) - 5.0;", *model10, *m_parser, m_args, m_output);
        model10->updateGraphAndOrderIfNeeded();

        auto model20 = assembly.findModel(20);
        model20->setDisplayName("shell");
        model20->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model20, *m_parser, m_args, m_output);
        model20->updateGraphAndOrderIfNeeded();

        auto snippet = ExpressionToGraphConverter::convertProgramToSnippet(assembly);

        // Mark function 20 as root
        std::set<nodes::ResourceId> rootIds = {20};
        auto annotated = ExpressionToGraphConverter::annotateRootFunctions(snippet, rootIds);

        // Function 20 should have [root] annotation
        EXPECT_NE(annotated.find("(ID: 20) [root]"), std::string::npos);
        // Function 10 should NOT have [root] annotation
        EXPECT_EQ(annotated.find("(ID: 10) [root]"), std::string::npos);
    }

    // =====================================================================================
    // T018: get_program_snippet snippet text includes [root] annotation on function headers
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, AnnotateRootFunctions_MultipleRoots_AnnotatesAll)
    {
        nodes::Assembly assembly;
        assembly.addModelIfNotExisting(10);
        assembly.addModelIfNotExisting(20);

        auto model10 = assembly.findModel(10);
        model10->setDisplayName("sphere");
        model10->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model10, *m_parser, m_args, m_output);
        model10->updateGraphAndOrderIfNeeded();

        auto model20 = assembly.findModel(20);
        model20->setDisplayName("shell");
        model20->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.y;", *model20, *m_parser, m_args, m_output);
        model20->updateGraphAndOrderIfNeeded();

        auto snippet = ExpressionToGraphConverter::convertProgramToSnippet(assembly);

        // Both functions are roots
        std::set<nodes::ResourceId> rootIds = {10, 20};
        auto annotated = ExpressionToGraphConverter::annotateRootFunctions(snippet, rootIds);

        EXPECT_NE(annotated.find("(ID: 10) [root]"), std::string::npos);
        EXPECT_NE(annotated.find("(ID: 20) [root]"), std::string::npos);
    }

    // =====================================================================================
    // T019: get_program_snippet includes non-root functions without [root] annotation
    // =====================================================================================

    TEST_F(MCPSnippetToolTest, AnnotateRootFunctions_EmptyRootSet_NoAnnotations)
    {
        nodes::Assembly assembly;
        assembly.addModelIfNotExisting(10);

        auto model10 = assembly.findModel(10);
        model10->setDisplayName("sphere");
        model10->createBeginEndWithDefaultInAndOuts();
        ExpressionToGraphConverter::convertSnippetToGraph(
          "return pos.x;", *model10, *m_parser, m_args, m_output);
        model10->updateGraphAndOrderIfNeeded();

        auto snippet = ExpressionToGraphConverter::convertProgramToSnippet(assembly);

        // No root functions
        std::set<nodes::ResourceId> rootIds;
        auto annotated = ExpressionToGraphConverter::annotateRootFunctions(snippet, rootIds);

        // Should be unchanged
        EXPECT_EQ(annotated, snippet);
        EXPECT_EQ(annotated.find("[root]"), std::string::npos);
    }

} // namespace gladius::tests
