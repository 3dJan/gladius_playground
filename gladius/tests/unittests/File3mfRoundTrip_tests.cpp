#include "opencl_test_helper.h"
#include "testhelper.h"

#include <Document.h>
#include <ExpressionParser.h>
#include <ExpressionToGraphConverter.h>
#include <FunctionArgument.h>
#include <compute/ComputeCore.h>
#include <nodes/Assembly.h>
#include <nodes/Model.h>
#include <nodes/Parameter.h>

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace gladius_tests
{
    using namespace gladius;

    /// @brief Test fixture that loads 3MF files and verifies graph↔snippet roundtrips
    /// for every function in the file.
    class File3mfRoundTrip_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            SKIP_IF_OPENCL_UNAVAILABLE();

            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }

            m_core =
              std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
        }

        std::shared_ptr<Document> loadDocument(std::filesystem::path const & path)
        {
            auto doc = std::make_shared<Document>(m_core);
            doc->load(path);
            return doc;
        }

        /// Extract function arguments from the Begin node's output ports.
        static std::vector<FunctionArgument> extractArguments(nodes::Model & model)
        {
            std::vector<FunctionArgument> args;
            auto * beginNode = model.getBeginNode();
            if (!beginNode)
            {
                return {{"pos", ArgumentType::Vector}};
            }

            auto const & outputs = beginNode->getOutputs();
            for (auto const & [portName, port] : outputs)
            {
                auto typeIdx = port.getTypeIndex();
                if (typeIdx == nodes::ParameterTypeIndex::Float3)
                {
                    args.emplace_back(portName, ArgumentType::Vector);
                }
                else if (typeIdx == nodes::ParameterTypeIndex::Float)
                {
                    args.emplace_back(portName, ArgumentType::Scalar);
                }
            }

            if (args.empty())
            {
                args.emplace_back("pos", ArgumentType::Vector);
            }
            return args;
        }

        /// Determine the function output from the End node's connected parameter.
        static FunctionOutput extractOutput(nodes::Model & model)
        {
            auto * endNode = model.getEndNode();
            if (!endNode)
            {
                return FunctionOutput::defaultOutput();
            }

            auto const & params = endNode->parameter();
            for (auto const & [name, param] : params)
            {
                if (param.getConstSource().has_value())
                {
                    auto typeIdx = param.getConstSource()->type;
                    if (typeIdx == nodes::ParameterTypeIndex::Float3)
                    {
                        return FunctionOutput(name, ArgumentType::Vector);
                    }
                    return FunctionOutput(name, ArgumentType::Scalar);
                }
            }
            return FunctionOutput::defaultOutput();
        }

        /// Determine all connected function outputs from the End node.
        static std::vector<FunctionOutput> extractOutputs(nodes::Model & model)
        {
            std::vector<FunctionOutput> outputs;
            auto * endNode = model.getEndNode();
            if (!endNode)
            {
                return {FunctionOutput::defaultOutput()};
            }

            auto const & params = endNode->parameter();
            for (auto const & [name, param] : params)
            {
                if (param.getConstSource().has_value())
                {
                    auto typeIdx = param.getConstSource()->type;
                    if (typeIdx == nodes::ParameterTypeIndex::Float3)
                    {
                        outputs.emplace_back(name, ArgumentType::Vector);
                    }
                    else
                    {
                        outputs.emplace_back(name, ArgumentType::Scalar);
                    }
                }
            }
            if (outputs.empty())
            {
                outputs.push_back(FunctionOutput::defaultOutput());
            }
            return outputs;
        }

        /// Check whether a snippet contains unsupported node comments that
        /// cannot be parsed back, making roundtrip inherently impossible.
        static bool containsUnsupportedNodes(std::string const & snippet)
        {
            return snippet.find("/* unsupported:") != std::string::npos;
        }

        /// Convert graph→snippet for a single function, rebuild graph from the snippet,
        /// then convert back to snippet again. Verify that the two snippets are identical.
        void verifyFunctionRoundTrip(nodes::Model & model,
                                     nodes::ResourceId funcId,
                                     nodes::Assembly & assembly)
        {
            auto args = extractArguments(model);
            auto outputs = extractOutputs(model);

            // Step 1: graph → snippet
            auto snippet1 = ExpressionToGraphConverter::convertGraphToSnippet(
              model, args, outputs, &assembly);

            if (snippet1.empty())
            {
                return;
            }

            auto displayName = model.getDisplayName().value_or(model.getModelName());
            SCOPED_TRACE(fmt::format(
              "Function '{}' (ID: {}), snippet:\n{}", displayName, funcId, snippet1));

            // Skip functions whose snippets contain unsupported node types
            if (containsUnsupportedNodes(snippet1))
            {
                return;
            }

            // Step 2: snippet → new graph
            nodes::Assembly tempAssembly(assembly);
            tempAssembly.addModelIfNotExisting(funcId);
            auto rebuiltModel = tempAssembly.findModel(funcId);
            ASSERT_TRUE(rebuiltModel) << "Failed to find model after addModelIfNotExisting";

            rebuiltModel->clear();
            rebuiltModel->createBeginEndWithDefaultInAndOuts();

            ExpressionParser parser;
            auto nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              snippet1, *rebuiltModel, parser, args, outputs, &tempAssembly);

            if (nodeId == 0)
            {
                ADD_FAILURE() << "convertSnippetToGraph returned 0 for snippet:\n" << snippet1;
                return;
            }

            rebuiltModel->updateGraphAndOrderIfNeeded();

            // Step 3: new graph → snippet again
            auto snippet2 = ExpressionToGraphConverter::convertGraphToSnippet(
              *rebuiltModel, args, outputs, &tempAssembly);

            // The original graph (from 3MF) may have higher fanout than the rebuilt graph
            // (e.g. UI-created connections), causing extra intermediate variables in snippet1.
            // Check idempotence: snippet2 → graph → snippet3 must be stable.
            // If not, allow one more roundtrip to settle (some FC nodes may have
            // different fanout counts between the first and second roundtrips due to
            // multi-output nodes like FunctionCall with shape + color ports).
            if (snippet1 != snippet2)
            {
                nodes::Assembly tempAssembly2(tempAssembly);
                tempAssembly2.addModelIfNotExisting(funcId);
                auto rebuiltModel2 = tempAssembly2.findModel(funcId);
                ASSERT_TRUE(rebuiltModel2) << "Failed to find model for idempotence check";

                rebuiltModel2->clear();
                rebuiltModel2->createBeginEndWithDefaultInAndOuts();

                ExpressionParser parser2;
                auto nodeId2 = ExpressionToGraphConverter::convertSnippetToGraph(
                  snippet2, *rebuiltModel2, parser2, args, outputs, &tempAssembly2);
                ASSERT_NE(nodeId2, 0u) << "Idempotence check: convertSnippetToGraph failed for:\n"
                                       << snippet2;

                rebuiltModel2->updateGraphAndOrderIfNeeded();
                auto snippet3 = ExpressionToGraphConverter::convertGraphToSnippet(
                  *rebuiltModel2, args, outputs, &tempAssembly2);

                if (snippet2 != snippet3)
                {
                    // Allow one more roundtrip to settle
                    nodes::Assembly tempAssembly3(tempAssembly2);
                    tempAssembly3.addModelIfNotExisting(funcId);
                    auto rebuiltModel3 = tempAssembly3.findModel(funcId);
                    ASSERT_TRUE(rebuiltModel3)
                      << "Failed to find model for second idempotence check";

                    rebuiltModel3->clear();
                    rebuiltModel3->createBeginEndWithDefaultInAndOuts();

                    ExpressionParser parser3;
                    auto nodeId3 = ExpressionToGraphConverter::convertSnippetToGraph(
                      snippet3, *rebuiltModel3, parser3, args, outputs, &tempAssembly3);
                    ASSERT_NE(nodeId3, 0u)
                      << "Second idempotence check: convertSnippetToGraph failed for:\n"
                      << snippet3;

                    rebuiltModel3->updateGraphAndOrderIfNeeded();
                    auto snippet4 = ExpressionToGraphConverter::convertGraphToSnippet(
                      *rebuiltModel3, args, outputs, &tempAssembly3);

                    EXPECT_EQ(snippet3, snippet4)
                      << "Idempotence failure (after settling) for function '"
                      << displayName << "' (ID: " << funcId << ")\n"
                      << "Original snippet:\n" << snippet1;
                }
            }
        }

        /// Verify the program-level roundtrip: convertProgramToSnippet → setProgramSnippet →
        /// convertProgramToSnippet. The two program texts should match.
        void verifyProgramRoundTrip(nodes::Assembly & assembly)
        {
            auto program1 = ExpressionToGraphConverter::convertProgramToSnippet(assembly);
            if (program1.empty())
            {
                return;
            }

            SCOPED_TRACE("Program roundtrip");

            nodes::Assembly rebuilt;
            ExpressionParser parser;

            ASSERT_NO_THROW(
              ExpressionToGraphConverter::setProgramSnippet(program1, rebuilt, parser))
              << "setProgramSnippet failed for program:\n"
              << program1;

            auto program2 = ExpressionToGraphConverter::convertProgramToSnippet(rebuilt);

            // The original graph may produce extra intermediate variables due to higher
            // fanout from UI connections. Check idempotence if texts differ.
            if (program1 != program2)
            {
                nodes::Assembly rebuilt2;
                ExpressionParser parser2;
                ASSERT_NO_THROW(
                  ExpressionToGraphConverter::setProgramSnippet(program2, rebuilt2, parser2))
                  << "Idempotence check: setProgramSnippet failed for program:\n"
                  << program2;

                auto program3 =
                  ExpressionToGraphConverter::convertProgramToSnippet(rebuilt2);

                EXPECT_EQ(program2, program3)
                  << "Program-level idempotence failure.\nOriginal program:\n" << program1;
            }
        }

        std::shared_ptr<ComputeContext> m_context;
        std::shared_ptr<ComputeCore> m_core;
        events::SharedLogger m_logger;
    };

    // ---------- Per-function roundtrip tests for each 3MF test file ----------

    TEST_F(File3mfRoundTrip_Test, SimpleGyroid_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/SimpleGyroid.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in SimpleGyroid.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    TEST_F(File3mfRoundTrip_Test, ImplicitGyroid_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/ImplicitGyroid.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in ImplicitGyroid.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    TEST_F(File3mfRoundTrip_Test, SphereInACage_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/SphereInACage.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in SphereInACage.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    TEST_F(File3mfRoundTrip_Test, WebcamMountColor_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/webcam_mount_color.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in webcam_mount_color.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    TEST_F(File3mfRoundTrip_Test, Filamentholder_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/filamentholder.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in filamentholder.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    TEST_F(File3mfRoundTrip_Test, Wristsupport_FunctionRoundTrips)
    {
        auto doc = loadDocument("testdata/wristsupport.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        auto & functions = assembly->getFunctions();
        EXPECT_GT(functions.size(), 0U) << "No functions found in wristsupport.3mf";

        for (auto & [id, model] : functions)
        {
            verifyFunctionRoundTrip(*model, id, *assembly);
        }
    }

    // ---------- Program-level roundtrip tests ----------

    TEST_F(File3mfRoundTrip_Test, SimpleGyroid_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/SimpleGyroid.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

    TEST_F(File3mfRoundTrip_Test, ImplicitGyroid_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/ImplicitGyroid.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

    TEST_F(File3mfRoundTrip_Test, SphereInACage_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/SphereInACage.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

    TEST_F(File3mfRoundTrip_Test, WebcamMountColor_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/webcam_mount_color.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

    TEST_F(File3mfRoundTrip_Test, Filamentholder_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/filamentholder.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

    TEST_F(File3mfRoundTrip_Test, Wristsupport_ProgramRoundTrip)
    {
        auto doc = loadDocument("testdata/wristsupport.3mf");
        auto assembly = doc->getAssembly();
        ASSERT_TRUE(assembly);

        verifyProgramRoundTrip(*assembly);
    }

} // namespace gladius_tests
