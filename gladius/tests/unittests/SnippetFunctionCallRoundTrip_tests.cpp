/**
 * @file SnippetFunctionCallRoundTrip_tests.cpp
 * @brief Roundtrip tests for FunctionCall nodes, cross-function references,
 *        program-level snippets, and under-tested node types (transform, matmul, select, etc.).
 *
 * These tests verify that:
 * 1. Graph → snippet produces correct code for FunctionCall/FunctionGradient nodes.
 * 2. Snippet → graph can reconstruct FunctionCall nodes (requires assembly context).
 * 3. Program-level convertProgramToSnippet ↔ setProgramSnippet roundtrip preserves topology.
 * 4. Under-tested built-in functions (transform, matmul, select, mat4, etc.) roundtrip correctly.
 */

#include <gtest/gtest.h>

#include "ExpressionParser.h"
#include "ExpressionToGraphConverter.h"
#include "FunctionArgument.h"
#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"

namespace gladius::tests
{
    // =====================================================================================
    // Helpers
    // =====================================================================================

    static std::vector<FunctionArgument> posArgs()
    {
        return {{"pos", ArgumentType::Vector}};
    }

    static FunctionOutput defaultOutput()
    {
        return FunctionOutput::defaultOutput();
    }

    /// Build a model from a snippet, return the normalized snippet.
    static std::string snippetRoundTrip(std::string const & snippet,
                                        std::vector<FunctionArgument> const & args,
                                        FunctionOutput const & output,
                                        nodes::Assembly * assembly = nullptr)
    {
        ExpressionParser parser;
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet, model, parser, args, output);
        if (nodeId == 0)
        {
            return {};
        }
        model.updateGraphAndOrderIfNeeded();
        return ExpressionToGraphConverter::convertGraphToSnippet(model, args, output, assembly);
    }

    /// Perform two roundtrips and check idempotency.
    static bool isIdempotent(std::string const & snippet,
                             std::vector<FunctionArgument> const & args,
                             FunctionOutput const & output)
    {
        auto s1 = snippetRoundTrip(snippet, args, output);
        if (s1.empty())
        {
            return false;
        }
        auto s2 = snippetRoundTrip(s1, args, output);
        return s1 == s2;
    }

    // =====================================================================================
    // FunctionCall: Graph → Snippet (tests nodeToExpression for FunctionCall nodes)
    // =====================================================================================

    class FunctionCallGraphToSnippetTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_assembly = std::make_unique<nodes::Assembly>();
        }

        /// Create a simple function in the assembly with the given expression body.
        void addFunction(nodes::ResourceId id,
                         std::string const & displayName,
                         std::string const & snippet)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEndWithDefaultInAndOuts();

            ExpressionParser parser;
            ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, *model, parser, posArgs(), defaultOutput());
            model->updateGraphAndOrderIfNeeded();
        }

        /// Create a function that calls another function and connects it to the End node.
        void addFunctionCallingOther(nodes::ResourceId id,
                                     std::string const & displayName,
                                     nodes::ResourceId calledId)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEndWithDefaultInAndOuts();

            auto calledModel = m_assembly->findModel(calledId);
            auto * fc = model->createFunctionCallNode(calledId, *calledModel);

            // Connect Begin.pos → FunctionCall.pos (the argument input)
            auto * beginNode = model->getBeginNode();
            if (beginNode)
            {
                auto * posOutput = beginNode->findOutputPort("pos");
                if (posOutput)
                {
                    auto const & fcParams = fc->parameter();
                    for (auto const & [name, param] : fcParams)
                    {
                        if (param.isArgument())
                        {
                            model->addLink(posOutput->getId(), param.getId());
                            break;
                        }
                    }
                }
            }

            // Connect FunctionCall output → End.shape
            auto * endNode = model->getEndNode();
            if (endNode)
            {
                auto * shapeParam = endNode->getParameter(nodes::FieldNames::Shape);
                if (shapeParam)
                {
                    auto & fcOutputs = fc->getOutputs();
                    for (auto & [name, port] : fcOutputs)
                    {
                        model->addLink(port.getId(), shapeParam->getId());
                        break;
                    }
                }
            }

            model->updateGraphAndOrderIfNeeded();
        }

        std::unique_ptr<nodes::Assembly> m_assembly;
    };

    TEST_F(FunctionCallGraphToSnippetTest, FunctionCall_GraphToSnippet_ProducesCorrectSyntax)
    {
        // Arrange: sphere (ID 10) is a leaf function, combo (ID 20) calls sphere
        addFunction(10, "sphere", "return length(pos) - 1.0;");
        addFunctionCallingOther(20, "combo", 10);

        // Act: convert combo's graph to a snippet with assembly context
        auto comboModel = m_assembly->findModel(20);
        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          *comboModel, posArgs(), defaultOutput(), m_assembly.get());

        // Assert: the snippet should contain the function call syntax sphere_10(...)
        EXPECT_FALSE(snippet.empty()) << "Snippet should not be empty";
        EXPECT_NE(snippet.find("sphere_10("), std::string::npos)
          << "Should contain sphere_10(...), got: [" << snippet << "]";
    }

    TEST_F(FunctionCallGraphToSnippetTest, FunctionCall_GraphToSnippet_ContainsReturnStatement)
    {
        addFunction(10, "box", "return max(max(abs(pos.x) - 1.0, abs(pos.y) - 1.0), abs(pos.z) - 1.0);");
        addFunctionCallingOther(20, "scene", 10);

        auto sceneModel = m_assembly->findModel(20);
        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          *sceneModel, posArgs(), defaultOutput(), m_assembly.get());

        EXPECT_NE(snippet.find("return"), std::string::npos)
          << "Snippet should contain return statement, got: [" << snippet << "]";
        EXPECT_NE(snippet.find("box_10("), std::string::npos)
          << "Should reference box_10(...), got: [" << snippet << "]";
    }

    TEST_F(FunctionCallGraphToSnippetTest, FunctionCall_WithoutAssembly_UsesFallbackName)
    {
        addFunction(10, "sphere", "return length(pos) - 1.0;");
        addFunctionCallingOther(20, "combo", 10);

        auto comboModel = m_assembly->findModel(20);
        // Convert WITHOUT assembly context — should fall back to func_10
        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          *comboModel, posArgs(), defaultOutput(), nullptr);

        EXPECT_FALSE(snippet.empty());
        EXPECT_NE(snippet.find("func_10("), std::string::npos)
          << "Without assembly, should use func_ID fallback, got: [" << snippet << "]";
    }

    // =====================================================================================
    // FunctionCall: Snippet → Graph (currently broken — documents the parsing gap)
    // These tests verify that convertSnippetToGraph can handle function call syntax.
    // =====================================================================================

    class FunctionCallSnippetToGraphTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_assembly = std::make_unique<nodes::Assembly>();
            m_parser = std::make_unique<ExpressionParser>();

            // Set up a simple function that the caller can reference
            m_assembly->addModelIfNotExisting(4);
            auto model = m_assembly->findModel(4);
            model->setDisplayName("box");
            model->createBeginEndWithDefaultInAndOuts();

            ExpressionParser parser;
            ExpressionToGraphConverter::convertSnippetToGraph(
              "return max(max(abs(pos.x) - 1.0, abs(pos.y) - 1.0), abs(pos.z) - 1.0);",
              *model, parser, posArgs(), defaultOutput());
            model->updateGraphAndOrderIfNeeded();
        }

        std::unique_ptr<nodes::Assembly> m_assembly;
        std::unique_ptr<ExpressionParser> m_parser;
    };

    TEST_F(FunctionCallSnippetToGraphTest, FunctionCall_ParseSnippet_CreatesGraphNode)
    {
        // This test documents the desired behavior:
        // Parsing "return box_4(pos);" should create a FunctionCall node referencing ID 4.
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return box_4(pos);", model, *m_parser, posArgs(), defaultOutput(), m_assembly.get());

        EXPECT_NE(nodeId, 0)
          << "convertSnippetToGraph should handle function call syntax box_4(pos)";
    }

    TEST_F(FunctionCallSnippetToGraphTest, FunctionCall_ParseWithArgs_CreatesCorrectNode)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return box_4(pos - vec3(5, 5, 5));", model, *m_parser, posArgs(), defaultOutput(), m_assembly.get());

        EXPECT_NE(nodeId, 0)
          << "Should parse box_4(pos - vec3(5, 5, 5))";
    }

    TEST_F(FunctionCallSnippetToGraphTest, FunctionCall_ParseWithVec3Arg_CreatesCorrectNode)
    {
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return box_4(vec3(10, 15, 10), pos - vec3(5, 5, 5));",
          model, *m_parser, posArgs(), defaultOutput(), m_assembly.get());

        EXPECT_NE(nodeId, 0)
          << "Should parse box_4(vec3(10, 15, 10), pos - vec3(5, 5, 5))";
    }

    // =====================================================================================
    // FunctionCall: Full roundtrip (graph → snippet → graph → snippet)
    // =====================================================================================

    TEST_F(FunctionCallGraphToSnippetTest, FunctionCall_FullRoundTrip_IsIdempotent)
    {
        // Arrange: create assembly with two functions
        addFunction(10, "sphere", "return length(pos) - 1.0;");
        addFunctionCallingOther(20, "combo", 10);

        auto comboModel = m_assembly->findModel(20);

        // Graph → Snippet (pass 1)
        auto snippet1 = ExpressionToGraphConverter::convertGraphToSnippet(
          *comboModel, posArgs(), defaultOutput(), m_assembly.get());
        ASSERT_FALSE(snippet1.empty()) << "First graph→snippet should succeed";
        ASSERT_NE(snippet1.find("sphere_10("), std::string::npos)
          << "Pass 1 should contain sphere_10(...), got: [" << snippet1 << "]";

        // Snippet → Graph (pass 2: parse the snippet back)
        ExpressionParser parser;
        nodes::Model rebuilt;
        rebuilt.createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet1, rebuilt, parser, posArgs(), defaultOutput(), m_assembly.get());

        EXPECT_NE(nodeId, 0)
          << "Parsing snippet with function call should succeed. Snippet: [" << snippet1 << "]";

        if (nodeId != 0)
        {
            rebuilt.updateGraphAndOrderIfNeeded();

            // Graph → Snippet again (pass 3) — should be idempotent
            auto snippet2 = ExpressionToGraphConverter::convertGraphToSnippet(
              rebuilt, posArgs(), defaultOutput(), m_assembly.get());
            EXPECT_EQ(snippet1, snippet2)
              << "Full roundtrip should be idempotent.\n"
              << "  Pass 1: [" << snippet1 << "]\n"
              << "  Pass 3: [" << snippet2 << "]";
        }
    }

    // =====================================================================================
    // Program-level roundtrip: convertProgramToSnippet ↔ setProgramSnippet
    // =====================================================================================

    class ProgramRoundTripTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_assembly = std::make_unique<nodes::Assembly>();
        }

        void addFunction(nodes::ResourceId id,
                         std::string const & displayName,
                         std::string const & snippet)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEndWithDefaultInAndOuts();

            ExpressionParser parser;
            ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, *model, parser, posArgs(), defaultOutput());
            model->updateGraphAndOrderIfNeeded();
        }

        void addFunctionCallingOther(nodes::ResourceId id,
                                     std::string const & displayName,
                                     nodes::ResourceId calledId)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEndWithDefaultInAndOuts();

            auto calledModel = m_assembly->findModel(calledId);
            model->createFunctionCallNode(calledId, *calledModel);

            model->updateGraphAndOrderIfNeeded();
        }

        std::unique_ptr<nodes::Assembly> m_assembly;
    };

    TEST_F(ProgramRoundTripTest, SingleFunction_ProgramRoundTrip_PreservesBody)
    {
        addFunction(10, "sphere", "return length(pos) - 1.0;");

        // Convert assembly → program listing
        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());
        EXPECT_NE(program.find("sphere_10"), std::string::npos);
        EXPECT_NE(program.find("// Function: sphere (ID: 10)"), std::string::npos);

        // Parse program listing back into a fresh assembly
        nodes::Assembly rebuilt;
        rebuilt.addModelIfNotExisting(10);
        rebuilt.findModel(10)->setDisplayName("sphere");
        ExpressionParser parser;
        ASSERT_NO_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, rebuilt, parser));

        // Convert the rebuilt assembly back to a program listing
        auto program2 = ExpressionToGraphConverter::convertProgramToSnippet(rebuilt);
        EXPECT_EQ(program, program2)
          << "Program-level roundtrip should be idempotent.\n"
          << "  Original:\n" << program << "\n"
          << "  Rebuilt:\n" << program2;
    }

    TEST_F(ProgramRoundTripTest, TwoIndependentFunctions_ProgramRoundTrip_PreservesBoth)
    {
        addFunction(10, "sphere", "return length(pos) - 1.0;");
        addFunction(20, "plane", "return pos.y;");

        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());
        EXPECT_NE(program.find("sphere_10"), std::string::npos);
        EXPECT_NE(program.find("plane_20"), std::string::npos);

        nodes::Assembly rebuilt;
        rebuilt.addModelIfNotExisting(10);
        rebuilt.findModel(10)->setDisplayName("sphere");
        rebuilt.addModelIfNotExisting(20);
        rebuilt.findModel(20)->setDisplayName("plane");
        ExpressionParser parser;
        ASSERT_NO_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, rebuilt, parser));

        auto program2 = ExpressionToGraphConverter::convertProgramToSnippet(rebuilt);
        EXPECT_EQ(program, program2)
          << "Two-function program roundtrip should be idempotent.\n"
          << "  Original:\n" << program << "\n"
          << "  Rebuilt:\n" << program2;
    }

    TEST_F(ProgramRoundTripTest, FunctionWithDependency_ProgramRoundTrip_PreservesCalls)
    {
        // sphere (ID 10) is a leaf; combo (ID 20) calls sphere
        addFunction(10, "sphere", "return length(pos) - 1.0;");
        addFunctionCallingOther(20, "combo", 10);

        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());

        // sphere should appear before combo (dependency order)
        auto spherePos = program.find("sphere_10");
        auto comboPos = program.find("combo_20");
        ASSERT_NE(spherePos, std::string::npos);
        ASSERT_NE(comboPos, std::string::npos);
        EXPECT_LT(spherePos, comboPos)
          << "sphere_10 should appear before combo_20 in dependency order";

        // combo's body should contain a call to sphere_10
        EXPECT_NE(program.find("sphere_10("), std::string::npos)
          << "combo body should call sphere_10(...), got:\n" << program;

        // Roundtrip: parse program back, verify it reproduces
        nodes::Assembly rebuilt;
        rebuilt.addModelIfNotExisting(10);
        rebuilt.findModel(10)->setDisplayName("sphere");
        rebuilt.addModelIfNotExisting(20);
        rebuilt.findModel(20)->setDisplayName("combo");
        ExpressionParser parser;

        // This will fail if setProgramSnippet can't parse function call syntax.
        // Currently expected to fail because convertSnippetToGraph can't handle
        // user-defined function calls like sphere_10(...).
        EXPECT_NO_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, rebuilt, parser))
          << "setProgramSnippet should handle cross-function calls";
    }

    // =====================================================================================
    // Under-tested built-in function roundtrips
    // =====================================================================================

    class BuiltinFunctionRoundTripTest : public ::testing::Test
    {
    };

    TEST_F(BuiltinFunctionRoundTripTest, Select_FourArgs_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {
          {"a", ArgumentType::Scalar},
          {"b", ArgumentType::Scalar},
          {"c", ArgumentType::Scalar},
          {"d", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return select(a, b, c, d);", args, defaultOutput()))
          << "select(a, b, c, d) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Atan2_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {
          {"y", ArgumentType::Scalar}, {"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return atan2(y, x);", args, defaultOutput()))
          << "atan2(y, x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Fmod_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return mod(x, 3.14);", args, defaultOutput()))
          << "mod(x, 3.14) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Floor_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return floor(x);", args, defaultOutput()))
          << "floor(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Ceil_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return ceil(x);", args, defaultOutput()))
          << "ceil(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Round_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return round(x);", args, defaultOutput()))
          << "round(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Fract_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return fract(x);", args, defaultOutput()))
          << "fract(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Sign_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return sign(x);", args, defaultOutput()))
          << "sign(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Length_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return length(pos);", posArgs(), defaultOutput()))
          << "length(pos) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Exp_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return exp(x);", args, defaultOutput()))
          << "exp(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Log_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return log(x);", args, defaultOutput()))
          << "log(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Log2_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return log2(x);", args, defaultOutput()))
          << "log2(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Log10_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return log10(x);", args, defaultOutput()))
          << "log10(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, SinH_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return sinh(x);", args, defaultOutput()))
          << "sinh(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, CosH_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return cosh(x);", args, defaultOutput()))
          << "cosh(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, TanH_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return tanh(x);", args, defaultOutput()))
          << "tanh(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, ArcSin_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return asin(x);", args, defaultOutput()))
          << "asin(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, ArcCos_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return acos(x);", args, defaultOutput()))
          << "acos(x) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, ArcTan_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return atan(x);", args, defaultOutput()))
          << "atan(x) should roundtrip";
    }

    // =====================================================================================
    // Composite expression roundtrips
    // =====================================================================================

    TEST_F(BuiltinFunctionRoundTripTest, LengthMinusSphere_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return length(pos) - 1.0;", posArgs(), defaultOutput()))
          << "length(pos) - 1.0 should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, DotSelf_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return dot(pos, pos);", posArgs(), defaultOutput()))
          << "dot(pos, pos) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, CrossSelf_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return cross(pos, pos);", posArgs(), defaultOutput()))
          << "cross(pos, pos) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Vec3Compose_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent(
          "return vec3(pos.x, pos.y, pos.z);", posArgs(), defaultOutput()))
          << "vec3(pos.x, pos.y, pos.z) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Vec3FromScalar_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"s", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return vec3(s);", args, defaultOutput()))
          << "vec3(s) should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, NestedClampMinMax_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent(
          "return clamp(min(x, 1.0), 0.0, max(x, 0.5));", args, defaultOutput()))
          << "Nested clamp/min/max should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, BoxSDF_WithVec3_RoundTrip_SnippetToGraph)
    {
        // This tests the exact pattern from the user's bug report
        ExpressionParser parser;
        nodes::Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return max(max(abs(pos.x) - 5.0, abs(pos.y) - 7.5), abs(pos.z) - 5.0);",
          model, parser, posArgs(), defaultOutput());
        ASSERT_NE(nodeId, 0) << "Box SDF expression should parse";

        model.updateGraphAndOrderIfNeeded();
        auto snippet = ExpressionToGraphConverter::convertGraphToSnippet(
          model, posArgs(), defaultOutput());
        EXPECT_FALSE(snippet.empty()) << "Graph→snippet should produce output";
        EXPECT_NE(snippet.find("return"), std::string::npos)
          << "Should contain return, got: [" << snippet << "]";
    }

    // =====================================================================================
    // generateUniqueFunctionName consistency tests
    // =====================================================================================

    class GenerateUniqueFunctionNameTest : public ::testing::Test
    {
    };

    TEST_F(GenerateUniqueFunctionNameTest, SimpleAlphanumericName_ProducesExpectedFormat)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("box", 4);
        EXPECT_EQ(result, "box_4");
    }

    TEST_F(GenerateUniqueFunctionNameTest, NameWithSpaces_SanitizesCorrectly)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("my box", 10);
        EXPECT_EQ(result, "my_box_10");
    }

    TEST_F(GenerateUniqueFunctionNameTest, NameStartingWithDigit_PrependsFPrefix)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("3dshape", 5);
        EXPECT_EQ(result, "f_3dshape_5");
    }

    TEST_F(GenerateUniqueFunctionNameTest, EmptyName_UsesFunc)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("", 7);
        EXPECT_EQ(result, "func_7");
    }

    TEST_F(GenerateUniqueFunctionNameTest, NameWithConsecutiveSpecialChars_CollapsesUnderscores)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("a--b", 1);
        EXPECT_EQ(result, "a_b_1");
    }

    TEST_F(GenerateUniqueFunctionNameTest, NameWithTrailingSpecialChars_TrimsUnderscore)
    {
        auto result = ExpressionToGraphConverter::generateUniqueFunctionName("box!", 3);
        EXPECT_EQ(result, "box_3");
    }

    // =====================================================================================
    // Edge cases
    // =====================================================================================

    TEST_F(BuiltinFunctionRoundTripTest, NegativeConstant_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return -1.0;", {}, defaultOutput()))
          << "Negative constant should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, Pi_RoundTrip_ParsesToConstant)
    {
        // pi is parsed as a constant — roundtrip may produce a numeric literal
        auto s1 = snippetRoundTrip("return pi;", {}, defaultOutput());
        EXPECT_FALSE(s1.empty()) << "pi should parse successfully";
    }

    TEST_F(BuiltinFunctionRoundTripTest, UnaryMinusThenFunction_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto s1 = snippetRoundTrip("return -sin(x);", args, defaultOutput());
        EXPECT_FALSE(s1.empty()) << "-sin(x) should parse";
        if (!s1.empty())
        {
            auto s2 = snippetRoundTrip(s1, args, defaultOutput());
            EXPECT_EQ(s1, s2)
              << "Unary minus + function should be idempotent.\n"
              << "  Pass 1: [" << s1 << "]\n"
              << "  Pass 2: [" << s2 << "]";
        }
    }

    TEST_F(BuiltinFunctionRoundTripTest, ComplexGyroid_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent(
          "return sin(pos.x) * cos(pos.y) + sin(pos.y) * cos(pos.z) + sin(pos.z) * cos(pos.x);",
          posArgs(), defaultOutput()))
          << "Gyroid expression should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, SharedSubexpression_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent(
          "float s = sin(x);\nreturn s * s;", args, defaultOutput()))
          << "Shared subexpression should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, ChainedAssignments_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent(
          "float a = x + 1.0;\nfloat b = a * 2.0;\nfloat c = b - 0.5;\nreturn c;",
          args, defaultOutput()))
          << "Chained assignments should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, DeeplyNestedFunctions_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent(
          "return sin(cos(sqrt(abs(x))));", args, defaultOutput()))
          << "Deeply nested functions should roundtrip";
    }

    TEST_F(BuiltinFunctionRoundTripTest, MultipleVectorComponents_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent(
          "return max(abs(pos.x), max(abs(pos.y), abs(pos.z)));",
          posArgs(), defaultOutput()))
          << "Multiple vector component accesses should roundtrip";
    }

} // namespace gladius::tests
