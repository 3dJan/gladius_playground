/**
 * @file SnippetGraphIdempotency_tests.cpp
 * @brief Tests that roundtrips between snippet (code) and graph representation are idempotent.
 *
 * After one normalization pass (snippet -> graph -> snippet), the resulting snippet must be
 * stable: feeding it back through the converter must produce the exact same snippet text.
 */

#include <gtest/gtest.h>

#include "ExpressionParser.h"
#include "ExpressionToGraphConverter.h"
#include "FunctionArgument.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "testhelper.h"

namespace gladius::tests
{
    /// Helper: run snippet -> graph -> snippet -> graph -> snippet and verify the last two
    /// snippets are identical (idempotency after normalization).
    struct RoundTripResult
    {
        std::string snippet1; ///< First normalized snippet  (pass 1: graph -> snippet)
        std::string snippet2; ///< Second normalized snippet (pass 2: graph -> snippet)
        bool pass1Ok = false;
        bool pass2Ok = false;
    };

    static RoundTripResult performRoundTrips(std::string const & originalSnippet,
                                             std::vector<FunctionArgument> const & args,
                                             FunctionOutput const & output)
    {
        RoundTripResult result;
        ExpressionParser parser;

        // Pass 1: original snippet -> graph1 -> snippet1
        {
            nodes::Model model1;
            model1.createBeginEndWithDefaultInAndOuts();
            auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              originalSnippet, model1, parser, args, output);
            result.pass1Ok = (nodeId != 0);
            if (!result.pass1Ok)
            {
                return result;
            }
            model1.updateGraphAndOrderIfNeeded();
            result.snippet1 =
              ExpressionToGraphConverter::convertGraphToSnippet(model1, args, output);
        }

        // Pass 2: snippet1 -> graph2 -> snippet2
        {
            nodes::Model model2;
            model2.createBeginEndWithDefaultInAndOuts();
            auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              result.snippet1, model2, parser, args, output);
            result.pass2Ok = (nodeId != 0);
            if (!result.pass2Ok)
            {
                return result;
            }
            model2.updateGraphAndOrderIfNeeded();
            result.snippet2 =
              ExpressionToGraphConverter::convertGraphToSnippet(model2, args, output);
        }

        return result;
    }

    /// Helper: count nodes in a model (excluding Begin/End).
    static size_t countContentNodes(nodes::Model & model)
    {
        size_t count = 0;
        for (auto it = model.begin(); it != model.end(); ++it)
        {
            auto * n = it->second.get();
            if (!n)
            {
                continue;
            }
            auto name = n->name();
            if (name != "Input" && name != "Output" && name != "Begin" && name != "End")
            {
                ++count;
            }
        }
        return count;
    }

    /// Helper: count links in a model.
    static size_t countLinks(nodes::Model & model)
    {
        size_t links = 0;
        for (auto it = model.begin(); it != model.end(); ++it)
        {
            auto * node = it->second.get();
            if (!node)
            {
                continue;
            }
            for (auto const & kv : node->constParameter())
            {
                if (kv.second.getConstSource().has_value())
                {
                    ++links;
                }
            }
        }
        return links;
    }

    // =====================================================================================
    // Idempotency Tests — snippet -> graph -> snippet must stabilize after one pass
    // =====================================================================================

    class SnippetGraphIdempotencyTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_parser = std::make_unique<ExpressionParser>();
        }

        std::unique_ptr<ExpressionParser> m_parser;

        static std::vector<FunctionArgument> posArgs()
        {
            return {{"pos", ArgumentType::Vector}};
        }

        static std::vector<FunctionArgument> posRadiusArgs()
        {
            return {{"pos", ArgumentType::Vector}, {"radius", ArgumentType::Scalar}};
        }

        static std::vector<FunctionArgument> scalarArgs()
        {
            return {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}};
        }

        static FunctionOutput scalarOutput()
        {
            return FunctionOutput::defaultOutput();
        }
    };

    TEST_F(SnippetGraphIdempotencyTest, SimpleAddition_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips("return a + b;", scalarArgs(), scalarOutput());
        ASSERT_TRUE(rt.pass1Ok) << "First pass should succeed";
        ASSERT_TRUE(rt.pass2Ok) << "Second pass (from normalized snippet) should succeed";
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Normalized snippet must be stable across roundtrips.\n"
          << "  Pass 1: [" << rt.snippet1 << "]\n"
          << "  Pass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, SimpleMultiplication_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips("return a * b;", scalarArgs(), scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, ChainedArithmetic_RoundTrip_IsIdempotent)
    {
        auto rt =
          performRoundTrips("return a + b * 2.0;", scalarArgs(), scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, TrigFunctions_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return sin(x) + cos(x);", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, VectorComponents_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips(
          "float d = sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);\nreturn d - 1.0;",
          posArgs(),
          scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, SphereWithRadius_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips(
          "float d = sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);\nreturn d - radius;",
          posRadiusArgs(),
          scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, MultiLineAssignments_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips(
          "float a = x * 2.0;\nfloat b = a + 1.0;\nreturn b;", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, NestedFunctions_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return sin(cos(x));", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, AbsFunction_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return abs(x);", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, ClampFunction_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return clamp(x, 0.0, 1.0);", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, MinMaxFunctions_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips("return min(a, max(b, 0.5));", scalarArgs(), scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, PowFunction_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return pow(x, 2.0);", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, Gyroid_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips(
          "return sin(pos.x) * cos(pos.y) + sin(pos.y) * cos(pos.z) + sin(pos.z) * cos(pos.x);",
          posArgs(),
          scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, BoxSDF_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips(
          "return max(max(abs(pos.x) - 1.0, abs(pos.y) - 1.0), abs(pos.z) - 1.0);",
          posArgs(),
          scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, ConstantOnly_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips("return 42.0;", {}, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, SharedSubexpression_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips(
          "float s = sin(x);\nreturn s + s;", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, ModFunction_RoundTrip_IsIdempotent)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        auto rt = performRoundTrips("return mod(x, 3.14);", args, scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, SubtractionAndDivision_RoundTrip_IsIdempotent)
    {
        auto rt = performRoundTrips("return (a - b) / 2.0;", scalarArgs(), scalarOutput());
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    // =====================================================================================
    // Graph topology preservation — verify that the graph produced from a normalized snippet
    // has the same node/link counts as the original graph.
    // =====================================================================================

    TEST_F(SnippetGraphIdempotencyTest, GraphTopology_SimpleExpression_PreservedAcrossRoundTrips)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        FunctionOutput output = scalarOutput();
        std::string snippet = "return sin(x) * 2.0 + 1.0;";

        // Build graph 1
        nodes::Model model1;
        model1.createBeginEndWithDefaultInAndOuts();
        auto id1 = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet, model1, *m_parser, args, output);
        ASSERT_NE(id1, 0);
        model1.updateGraphAndOrderIfNeeded();

        auto snippet1 = ExpressionToGraphConverter::convertGraphToSnippet(model1, args, output);
        ASSERT_FALSE(snippet1.empty());

        size_t nodes1 = countContentNodes(model1);
        size_t links1 = countLinks(model1);

        // Build graph 2 from the normalized snippet
        nodes::Model model2;
        model2.createBeginEndWithDefaultInAndOuts();
        auto id2 = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet1, model2, *m_parser, args, output);
        ASSERT_NE(id2, 0);
        model2.updateGraphAndOrderIfNeeded();

        size_t nodes2 = countContentNodes(model2);
        size_t links2 = countLinks(model2);

        EXPECT_EQ(nodes1, nodes2) << "Content node count should be identical after roundtrip";
        EXPECT_EQ(links1, links2) << "Link count should be identical after roundtrip";
    }

    TEST_F(SnippetGraphIdempotencyTest, GraphTopology_Gyroid_PreservedAcrossRoundTrips)
    {
        auto args = posArgs();
        FunctionOutput output = scalarOutput();
        std::string snippet =
          "return sin(pos.x) * cos(pos.y) + sin(pos.y) * cos(pos.z) + sin(pos.z) * cos(pos.x);";

        nodes::Model model1;
        model1.createBeginEndWithDefaultInAndOuts();
        auto id1 = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet, model1, *m_parser, args, output);
        ASSERT_NE(id1, 0);
        model1.updateGraphAndOrderIfNeeded();

        auto snippet1 = ExpressionToGraphConverter::convertGraphToSnippet(model1, args, output);
        ASSERT_FALSE(snippet1.empty());

        size_t nodes1 = countContentNodes(model1);
        size_t links1 = countLinks(model1);

        nodes::Model model2;
        model2.createBeginEndWithDefaultInAndOuts();
        auto id2 = ExpressionToGraphConverter::convertSnippetToGraph(
          snippet1, model2, *m_parser, args, output);
        ASSERT_NE(id2, 0);
        model2.updateGraphAndOrderIfNeeded();

        size_t nodes2 = countContentNodes(model2);
        size_t links2 = countLinks(model2);

        EXPECT_EQ(nodes1, nodes2) << "Node count mismatch for gyroid roundtrip";
        EXPECT_EQ(links1, links2) << "Link count mismatch for gyroid roundtrip";
    }

    // =====================================================================================
    // Expression roundtrip — expression -> graph -> snippet -> graph -> snippet must stabilize
    // =====================================================================================

    TEST_F(SnippetGraphIdempotencyTest, ExpressionOrigin_Simple_BecomesIdempotentAfterFirstPass)
    {
        std::vector<FunctionArgument> args = {{"x", ArgumentType::Scalar}};
        FunctionOutput output = scalarOutput();

        // Start from a raw expression (not a snippet)
        nodes::Model model1;
        model1.createBeginEndWithDefaultInAndOuts();
        auto id1 = ExpressionToGraphConverter::convertExpressionToGraph(
          "sin(x) + cos(x)", model1, *m_parser, args, output);
        ASSERT_NE(id1, 0);
        model1.updateGraphAndOrderIfNeeded();

        std::string snippet1 =
          ExpressionToGraphConverter::convertGraphToSnippet(model1, args, output);
        ASSERT_FALSE(snippet1.empty());

        // Now roundtrip the snippet
        auto rt = performRoundTrips(snippet1, args, output);
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Expression-originated snippet should be stable after first normalization.\n"
          << "  Pass 1: [" << rt.snippet1 << "]\n"
          << "  Pass 2: [" << rt.snippet2 << "]";
    }

    TEST_F(SnippetGraphIdempotencyTest, ExpressionOrigin_VectorComponents_BecomesIdempotent)
    {
        auto args = posArgs();
        FunctionOutput output = scalarOutput();

        nodes::Model model1;
        model1.createBeginEndWithDefaultInAndOuts();
        auto id1 = ExpressionToGraphConverter::convertExpressionToGraph(
          "pos.x + pos.y + pos.z", model1, *m_parser, args, output);
        ASSERT_NE(id1, 0);
        model1.updateGraphAndOrderIfNeeded();

        std::string snippet1 =
          ExpressionToGraphConverter::convertGraphToSnippet(model1, args, output);
        ASSERT_FALSE(snippet1.empty());

        auto rt = performRoundTrips(snippet1, args, output);
        ASSERT_TRUE(rt.pass1Ok);
        ASSERT_TRUE(rt.pass2Ok);
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Pass 1: [" << rt.snippet1 << "]\nPass 2: [" << rt.snippet2 << "]";
    }

    // =====================================================================================
    // Three-pass stability — verify convergence even from arbitrary input
    // =====================================================================================

    TEST_F(SnippetGraphIdempotencyTest, ThreePassStability_ComplexExpression_ConvergesWithinTwoPasses)
    {
        auto args = posRadiusArgs();
        FunctionOutput output = scalarOutput();

        // Somewhat complex input that may get restructured
        std::string original =
          "float dx = pos.x * pos.x;\n"
          "float dy = pos.y * pos.y;\n"
          "float dz = pos.z * pos.z;\n"
          "float d = sqrt(dx + dy + dz);\n"
          "return d - radius;";

        ExpressionParser parser;

        // Pass 1
        nodes::Model m1;
        m1.createBeginEndWithDefaultInAndOuts();
        auto id1 =
          ExpressionToGraphConverter::convertSnippetToGraph(original, m1, parser, args, output);
        ASSERT_NE(id1, 0);
        m1.updateGraphAndOrderIfNeeded();
        std::string s1 = ExpressionToGraphConverter::convertGraphToSnippet(m1, args, output);
        ASSERT_FALSE(s1.empty());

        // Pass 2
        nodes::Model m2;
        m2.createBeginEndWithDefaultInAndOuts();
        auto id2 =
          ExpressionToGraphConverter::convertSnippetToGraph(s1, m2, parser, args, output);
        ASSERT_NE(id2, 0);
        m2.updateGraphAndOrderIfNeeded();
        std::string s2 = ExpressionToGraphConverter::convertGraphToSnippet(m2, args, output);
        ASSERT_FALSE(s2.empty());

        // Pass 3
        nodes::Model m3;
        m3.createBeginEndWithDefaultInAndOuts();
        auto id3 =
          ExpressionToGraphConverter::convertSnippetToGraph(s2, m3, parser, args, output);
        ASSERT_NE(id3, 0);
        m3.updateGraphAndOrderIfNeeded();
        std::string s3 = ExpressionToGraphConverter::convertGraphToSnippet(m3, args, output);
        ASSERT_FALSE(s3.empty());

        // The output must have converged by pass 2 at the latest
        EXPECT_EQ(s2, s3)
          << "Snippet must converge within two normalization passes.\n"
          << "  Pass 2: [" << s2 << "]\n"
          << "  Pass 3: [" << s3 << "]";
    }

    // =====================================================================================
    // Parameterized idempotency test for broader coverage
    // =====================================================================================

    struct IdempotencyTestCase
    {
        std::string name;
        std::string snippet;
        std::vector<FunctionArgument> args;
    };

    class ParameterizedIdempotencyTest
        : public ::testing::TestWithParam<IdempotencyTestCase>
    {
    };

    TEST_P(ParameterizedIdempotencyTest, RoundTrip_IsIdempotent)
    {
        auto const & tc = GetParam();
        auto rt = performRoundTrips(tc.snippet, tc.args, FunctionOutput::defaultOutput());
        ASSERT_TRUE(rt.pass1Ok) << "First pass failed for: " << tc.name;
        ASSERT_TRUE(rt.pass2Ok) << "Second pass failed for: " << tc.name;
        EXPECT_EQ(rt.snippet1, rt.snippet2)
          << "Idempotency failed for: " << tc.name << "\n"
          << "  Pass 1: [" << rt.snippet1 << "]\n"
          << "  Pass 2: [" << rt.snippet2 << "]";
    }

    // clang-format off
    INSTANTIATE_TEST_SUITE_P(
      SnippetIdempotency,
      ParameterizedIdempotencyTest,
      ::testing::Values(
        IdempotencyTestCase{"Constant",         "return 1.0;",                          {}},
        IdempotencyTestCase{"ScalarAdd",         "return a + b;",                       {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ScalarSub",         "return a - b;",                       {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ScalarMul",         "return a * b;",                       {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ScalarDiv",         "return a / b;",                       {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"SinCos",            "return sin(x) + cos(x);",             {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Sqrt",              "return sqrt(x);",                     {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Abs",               "return abs(x);",                      {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Pow",               "return pow(x, 3.0);",                 {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Clamp",             "return clamp(x, 0.0, 1.0);",          {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Min",               "return min(a, b);",                   {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Max",               "return max(a, b);",                   {{"a", ArgumentType::Scalar}, {"b", ArgumentType::Scalar}}},
        IdempotencyTestCase{"Mod",               "return mod(x, 2.0);",                 {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"VecX",              "return pos.x;",                       {{"pos", ArgumentType::Vector}}},
        IdempotencyTestCase{"VecXYZ",            "return pos.x + pos.y + pos.z;",       {{"pos", ArgumentType::Vector}}},
        IdempotencyTestCase{"NestedSinCos",      "return sin(cos(x));",                 {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"SphereFromComps",   "return sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z) - 1.0;", {{"pos", ArgumentType::Vector}}},
        IdempotencyTestCase{"BoxApprox",         "return max(max(abs(pos.x) - 1.0, abs(pos.y) - 1.0), abs(pos.z) - 1.0);", {{"pos", ArgumentType::Vector}}},
        IdempotencyTestCase{"SineWave",          "return pos.y - sin(pos.x * 3.14);",   {{"pos", ArgumentType::Vector}}},
        IdempotencyTestCase{"MultiAssign",       "float a = x * 2.0;\nreturn a + 1.0;", {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ChainedAssign",     "float a = x + 1.0;\nfloat b = a * 2.0;\nreturn b;", {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ScientificNotPos",  "return x + 1000000;",                 {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"ScientificNotNeg",  "return x * 0.000001;",                {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"LargeConstMul",     "return x * 1000000 + 0.001;",         {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"PiConstant",        "return x * pi;",                      {{"x", ArgumentType::Scalar}}},
        IdempotencyTestCase{"PiInExpr",          "return sin(x * pi / 180);",           {{"x", ArgumentType::Scalar}}}
      ),
      [](::testing::TestParamInfo<IdempotencyTestCase> const & info)
      { return info.param.name; });
    // clang-format on

} // namespace gladius::tests
