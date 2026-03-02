/**
 * @file GraphToSnippet_tests.cpp
 * @brief Tests for graph-to-snippet conversion of new node types
 *        (vector, matrix, function call, resource-backed nodes).
 */

#include <gtest/gtest.h>

#include "ExpressionParser.h"
#include "ExpressionToGraphConverter.h"
#include "FunctionArgument.h"
#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/nodesfwd.h"

namespace gladius::tests
{
    class GraphToSnippetNewNodeTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_model = std::make_unique<nodes::Model>();
            m_parser = std::make_unique<ExpressionParser>();
        }

        /// Build a graph from a snippet expression, then convert back to snippet
        std::string roundTripSnippet(std::string const & snippet,
                                     std::vector<FunctionArgument> const & args,
                                     FunctionOutput const & output)
        {
            m_model->createBeginEndWithDefaultInAndOuts();
            auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, *m_model, *m_parser, args, output);
            if (nodeId == 0)
            {
                return {};
            }
            m_model->updateGraphAndOrderIfNeeded();
            return ExpressionToGraphConverter::convertGraphToSnippet(*m_model, args, output);
        }

        static std::vector<FunctionArgument> posArgs()
        {
            return {{"pos", ArgumentType::Vector}};
        }

        static std::vector<FunctionArgument> scalarArgs()
        {
            return {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}};
        }

        static FunctionOutput scalarOutput()
        {
            return FunctionOutput::defaultOutput();
        }

        std::unique_ptr<nodes::Model> m_model;
        std::unique_ptr<ExpressionParser> m_parser;
    };

    // =====================================================================================
    // T024: Vector types — ComposeVector, VectorFromScalar
    // =====================================================================================

    TEST_F(GraphToSnippetNewNodeTest, ComposeVector_ThreeScalars_ProducesVec3)
    {
        auto result = roundTripSnippet(
          "return vec3(pos.x, pos.y, pos.z);", posArgs(), scalarOutput());
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("vec3("), std::string::npos)
          << "Should contain vec3(), got: [" << result << "]";
    }

    TEST_F(GraphToSnippetNewNodeTest, VectorFromScalar_SingleArg_ProducesVec3)
    {
        std::vector<FunctionArgument> args = {{"s", ArgumentType::Scalar}};
        auto result = roundTripSnippet("return vec3(s);", args, scalarOutput());
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("vec3("), std::string::npos)
          << "Should contain vec3(), got: [" << result << "]";
    }

    // =====================================================================================
    // T025: Matrix operations — Transpose, Inverse, MatrixVectorMultiplication
    // =====================================================================================

    TEST_F(GraphToSnippetNewNodeTest, DotProduct_TwoVectors_ProducesDotCall)
    {
        auto result =
          roundTripSnippet("return dot(pos, pos);", posArgs(), scalarOutput());
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("dot("), std::string::npos)
          << "Should contain dot(), got: [" << result << "]";
    }

    TEST_F(GraphToSnippetNewNodeTest, CrossProduct_TwoVectors_ProducesCrossCall)
    {
        auto result =
          roundTripSnippet("return cross(pos, pos);", posArgs(), scalarOutput());
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("cross("), std::string::npos)
          << "Should contain cross(), got: [" << result << "]";
    }

    // =====================================================================================
    // T028: Snippet→Graph parsing of new function syntax
    // =====================================================================================

    TEST_F(GraphToSnippetNewNodeTest, Vec3Parse_ThreeArgs_CreatesComposeVector)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return vec3(pos.x, pos.y, pos.z);", *m_model, *m_parser, posArgs(), scalarOutput());
        EXPECT_NE(nodeId, 0) << "Parsing vec3(x, y, z) should succeed";
    }

    TEST_F(GraphToSnippetNewNodeTest, Vec3Parse_SingleArg_CreatesVectorFromScalar)
    {
        std::vector<FunctionArgument> args = {{"s", ArgumentType::Scalar}};
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return vec3(s);", *m_model, *m_parser, args, scalarOutput());
        EXPECT_NE(nodeId, 0) << "Parsing vec3(s) should succeed";
    }

    TEST_F(GraphToSnippetNewNodeTest, DotParse_TwoArgs_CreatesDotProduct)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return dot(pos, pos);", *m_model, *m_parser, posArgs(), scalarOutput());
        EXPECT_NE(nodeId, 0) << "Parsing dot(a, b) should succeed";
    }

    TEST_F(GraphToSnippetNewNodeTest, CrossParse_TwoArgs_CreatesCrossProduct)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return cross(pos, pos);", *m_model, *m_parser, posArgs(), scalarOutput());
        EXPECT_NE(nodeId, 0) << "Parsing cross(a, b) should succeed";
    }

    // =====================================================================================
    // T031: Idempotency tests for new node types
    // =====================================================================================

    static std::string normalizeSnippet(std::string const & snippet,
                                        std::vector<FunctionArgument> const & args,
                                        FunctionOutput const & output)
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
        return ExpressionToGraphConverter::convertGraphToSnippet(model, args, output);
    }

    static bool isIdempotent(std::string const & snippet,
                             std::vector<FunctionArgument> const & args,
                             FunctionOutput const & output)
    {
        auto s1 = normalizeSnippet(snippet, args, output);
        if (s1.empty())
        {
            return false;
        }
        auto s2 = normalizeSnippet(s1, args, output);
        return s1 == s2;
    }

    TEST_F(GraphToSnippetNewNodeTest, ComposeVector_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(isIdempotent("return vec3(pos.x, pos.y, pos.z);",
                                 posArgs(), scalarOutput()));
    }

    TEST_F(GraphToSnippetNewNodeTest, VectorFromScalar_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"s", ArgumentType::Scalar}};
        EXPECT_TRUE(isIdempotent("return vec3(s);", args, scalarOutput()));
    }

    TEST_F(GraphToSnippetNewNodeTest, DotProduct_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(
          isIdempotent("return dot(pos, pos);", posArgs(), scalarOutput()));
    }

    TEST_F(GraphToSnippetNewNodeTest, CrossProduct_RoundTrip_IsIdempotent)
    {
        EXPECT_TRUE(
          isIdempotent("return cross(pos, pos);", posArgs(), scalarOutput()));
    }

    // =====================================================================================
    // T058: convertProgramToSnippet produces all functions in dependency order
    // =====================================================================================

    class ProgramSnippetTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_assembly = std::make_unique<nodes::Assembly>();
        }

        /// Helper: add a function with a simple expression body
        void addFunction(nodes::ResourceId id,
                         std::string const & displayName,
                         std::string const & snippet)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEndWithDefaultInAndOuts();

            ExpressionParser parser;
            std::vector<FunctionArgument> args = {{"pos", ArgumentType::Vector}};
            ExpressionToGraphConverter::convertSnippetToGraph(
              snippet, *model, parser, args, FunctionOutput::defaultOutput());
            model->updateGraphAndOrderIfNeeded();
        }

        /// Helper: add a function that calls another function
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
            (void)fc;

            model->updateGraphAndOrderIfNeeded();
        }

        std::unique_ptr<nodes::Assembly> m_assembly;
    };

    TEST_F(ProgramSnippetTest, AllFunctions_AppearInDependencyOrder)
    {
        // sphere (ID 10) has no dependencies
        addFunction(10, "sphere", "return pos.x + pos.y;");

        // combo (ID 20) calls sphere — should appear after sphere
        addFunctionCallingOther(20, "combo", 10);

        auto result = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        EXPECT_FALSE(result.empty());

        // sphere should appear before combo
        auto spherePos = result.find("sphere_10");
        auto comboPos = result.find("combo_20");
        EXPECT_NE(spherePos, std::string::npos) << "Should contain sphere_10, got:\n" << result;
        EXPECT_NE(comboPos, std::string::npos) << "Should contain combo_20, got:\n" << result;
        EXPECT_LT(spherePos, comboPos) << "sphere_10 should appear before combo_20";

        // Both function headers should be present
        EXPECT_NE(result.find("// Function: sphere (ID: 10)"), std::string::npos);
        EXPECT_NE(result.find("// Function: combo (ID: 20)"), std::string::npos);
    }

    // =====================================================================================
    // T059: convertProgramToSnippet detects circular dependencies
    // =====================================================================================

    TEST_F(ProgramSnippetTest, CircularDependency_Throws)
    {
        // Create two functions that call each other
        m_assembly->addModelIfNotExisting(10);
        m_assembly->addModelIfNotExisting(20);

        auto model10 = m_assembly->findModel(10);
        model10->setDisplayName("funcA");
        model10->createBeginEndWithDefaultInAndOuts();

        auto model20 = m_assembly->findModel(20);
        model20->setDisplayName("funcB");
        model20->createBeginEndWithDefaultInAndOuts();

        // funcA calls funcB
        auto * fc1 = model10->create<nodes::FunctionCall>();
        fc1->setFunctionId(20);

        // funcB calls funcA
        auto * fc2 = model20->create<nodes::FunctionCall>();
        fc2->setFunctionId(10);

        EXPECT_THROW(
          ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly),
          std::runtime_error);
    }

    // =====================================================================================
    // T060: Functions with same display name get distinct unique names
    // =====================================================================================

    TEST_F(ProgramSnippetTest, SameDisplayName_DistinctUniqueNames)
    {
        addFunction(10, "sphere", "return pos.x;");
        addFunction(20, "sphere", "return pos.y;");

        auto result = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);

        EXPECT_NE(result.find("sphere_10"), std::string::npos)
          << "Should contain sphere_10, got:\n" << result;
        EXPECT_NE(result.find("sphere_20"), std::string::npos)
          << "Should contain sphere_20, got:\n" << result;
    }

    // =====================================================================================
    // mat4 support: parsing, arguments, return types, and roundtrip
    // =====================================================================================

    TEST_F(GraphToSnippetNewNodeTest, Mat4Constructor_16Literals_CreatesConstantMatrix)
    {
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return mat4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);",
          *m_model, *m_parser, posArgs(), scalarOutput());
        EXPECT_NE(nodeId, 0) << "Parsing mat4(16 floats) should succeed";
    }

    TEST_F(GraphToSnippetNewNodeTest, Mat4Constructor_RoundTrip_ProducesMat4)
    {
        FunctionOutput matOutput("result", ArgumentType::Matrix);
        auto result = roundTripSnippet(
          "return mat4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);",
          posArgs(), matOutput);
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("mat4("), std::string::npos)
          << "Should contain mat4(), got: [" << result << "]";
    }

    TEST_F(GraphToSnippetNewNodeTest, Mat4Argument_ParsedInSignature_CreatesBeginNodeParam)
    {
        std::vector<FunctionArgument> args = {{"m", ArgumentType::Matrix}};
        m_model->createBeginEndWithDefaultInAndOuts();
        auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
          "return transpose(m);", *m_model, *m_parser, args,
          FunctionOutput("result", ArgumentType::Matrix));
        EXPECT_NE(nodeId, 0) << "Parsing with mat4 argument should succeed";
    }

    TEST_F(GraphToSnippetNewNodeTest, Mat4Argument_RoundTrip_PreservesType)
    {
        std::vector<FunctionArgument> args = {{"m", ArgumentType::Matrix}};
        FunctionOutput matOutput("result", ArgumentType::Matrix);
        auto result = roundTripSnippet("return transpose(m);", args, matOutput);
        EXPECT_FALSE(result.empty()) << "Round-trip should succeed";
        EXPECT_NE(result.find("transpose("), std::string::npos)
          << "Should contain transpose(), got: [" << result << "]";
    }

    class Mat4ProgramRoundTripTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_assembly = std::make_unique<nodes::Assembly>();
        }

        void addMatrixFunction(nodes::ResourceId id,
                               std::string const & displayName,
                               std::string const & body,
                               std::vector<FunctionArgument> const & args,
                               std::vector<FunctionOutput> const & outputs)
        {
            m_assembly->addModelIfNotExisting(id);
            auto model = m_assembly->findModel(id);
            model->setDisplayName(displayName);
            model->createBeginEnd();

            ExpressionParser parser;
            ExpressionToGraphConverter::convertSnippetToGraph(
              body, *model, parser, args, outputs, nullptr);
            model->updateGraphAndOrderIfNeeded();
        }

        std::unique_ptr<nodes::Assembly> m_assembly;
    };

    TEST_F(Mat4ProgramRoundTripTest, Mat4ReturnType_ProgramSnippet_ShowsMat4InSignature)
    {
        addMatrixFunction(
          10, "identity", "return mat4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);",
          {{"pos", ArgumentType::Vector}},
          {FunctionOutput("result", ArgumentType::Matrix)});

        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());
        EXPECT_NE(program.find("mat4 identity_10"), std::string::npos)
          << "Return type should be mat4, got:\n" << program;
    }

    TEST_F(Mat4ProgramRoundTripTest, Mat4Argument_ProgramSnippet_ShowsMat4InSignature)
    {
        addMatrixFunction(
          10, "transposeFunc", "return transpose(m);",
          {{"m", ArgumentType::Matrix}},
          {FunctionOutput("result", ArgumentType::Matrix)});

        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());
        EXPECT_NE(program.find("mat4 m"), std::string::npos)
          << "Argument type should be mat4, got:\n" << program;
    }

    TEST_F(Mat4ProgramRoundTripTest, Mat4Function_ProgramRoundTrip_IsIdempotent)
    {
        addMatrixFunction(
          10, "identity", "return mat4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);",
          {{"pos", ArgumentType::Vector}},
          {FunctionOutput("result", ArgumentType::Matrix)});

        auto program = ExpressionToGraphConverter::convertProgramToSnippet(*m_assembly);
        ASSERT_FALSE(program.empty());

        nodes::Assembly rebuilt;
        rebuilt.addModelIfNotExisting(10);
        rebuilt.findModel(10)->setDisplayName("identity");
        ExpressionParser parser;
        ASSERT_NO_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, rebuilt, parser));

        auto program2 = ExpressionToGraphConverter::convertProgramToSnippet(rebuilt);
        EXPECT_EQ(program, program2)
          << "mat4 program roundtrip should be idempotent.\n"
          << "  Original:\n" << program << "\n"
          << "  Rebuilt:\n" << program2;
    }

    TEST_F(Mat4ProgramRoundTripTest, Mat4ArgParsing_SetProgramSnippet_ParsesMat4Type)
    {
        // Manually construct a program snippet with mat4 argument
        std::string program =
          "// Function: transposeFunc (ID: 10)\n"
          "mat4 transposeFunc_10(mat4 m) {\n"
          "  return transpose(m);\n"
          "}\n";

        nodes::Assembly rebuilt;
        rebuilt.addModelIfNotExisting(10);
        rebuilt.findModel(10)->setDisplayName("transposeFunc");
        ExpressionParser parser;
        ASSERT_NO_THROW(
          ExpressionToGraphConverter::setProgramSnippet(program, rebuilt, parser));

        auto program2 = ExpressionToGraphConverter::convertProgramToSnippet(rebuilt);
        EXPECT_NE(program2.find("mat4 m"), std::string::npos)
          << "Should preserve mat4 argument type, got:\n" << program2;
        EXPECT_NE(program2.find("mat4 transposeFunc_10"), std::string::npos)
          << "Should preserve mat4 return type, got:\n" << program2;
    }

} // namespace gladius::tests
