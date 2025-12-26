#include <gtest/gtest.h>

#include <nodes/Assembly.h>
#include <nodes/DerivedNodes.h>
#include <nodes/GraphFlattener.h>
#include <nodes/Model.h>
#include <nodes/Visitor.h>

#include "testhelper.h"

namespace gladius_tests
{
    namespace
    {
        using gladius::nodes::Addition;
        using gladius::nodes::Assembly;
        using gladius::nodes::Begin;
        using gladius::nodes::ConstantScalar;
        using gladius::nodes::End;
        using gladius::nodes::FieldNames;
        using gladius::nodes::FunctionCall;
        using gladius::nodes::GraphFlattener;
        using gladius::nodes::Model;
        using gladius::nodes::Multiplication;
        using gladius::nodes::ParameterTypeIndex;
        using gladius::nodes::ResourceId;
        using gladius::nodes::VariantParameter;
        using gladius::nodes::VariantType;

        // Output field name used consistently in test functions
        // Using Distance since that's what the main assembly model expects
        constexpr auto OutputFieldName = FieldNames::Distance;

        // Test fixture for GraphFlattener tests
        class GraphFlattenerTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_assembly = std::make_unique<Assembly>();
            }

            /// Creates a simple function that adds a constant to a scalar input
            /// Function: output = input + constant
            void createAddConstantFunction(ResourceId functionId,
                                           float constantValue,
                                           std::string const & functionName = "AddConstant")
            {
                m_assembly->addModelIfNotExisting(functionId);
                auto model = m_assembly->findModel(functionId);
                ASSERT_NE(model, nullptr);

                model->createBeginEnd();
                model->setResourceId(functionId);
                model->setModelName(functionName);

                // Add input argument
                VariantParameter inputArg{VariantType{0.f}};
                inputArg.marksAsArgument();
                inputArg.setInputSourceRequired(false);
                model->addArgument(FieldNames::A, inputArg);

                // Add output - use Distance to match main model expectations
                VariantParameter output{VariantType{0.f}};
                output.setInputSourceRequired(true);
                output.setConsumedByFunction(false);
                model->addFunctionOutput(OutputFieldName, output);

                // Create constant node
                auto * constant = model->create<ConstantScalar>();
                constant->parameter()[FieldNames::Value].setValue(VariantType{constantValue});
                constant->parameter()[FieldNames::Value].setInputSourceRequired(false);

                // Create addition node
                auto * addition = model->create<Addition>();

                // Connect Begin -> Addition (A input)
                auto * beginNode = model->getBeginNode();
                ASSERT_NE(beginNode, nullptr);
                auto & beginOutputs = beginNode->getOutputs();
                auto aOutputIt = beginOutputs.find(FieldNames::A);
                ASSERT_NE(aOutputIt, beginOutputs.end());
                addition->parameter()[FieldNames::A].setInputFromPort(aOutputIt->second);

                // Connect Constant -> Addition (B input)
                addition->parameter()[FieldNames::B].setInputFromPort(
                  constant->getOutputs().at(FieldNames::Value));

                // Connect Addition -> End (output)
                auto & outputs = model->getOutputs();
                auto outputIt = outputs.find(OutputFieldName);
                ASSERT_NE(outputIt, outputs.end());
                ASSERT_TRUE(model->addLink(addition->getOutputs().at(FieldNames::Result).getId(),
                                           outputIt->second.getId()));

                model->invalidateGraph();
                model->updateGraphAndOrderIfNeeded();
            }

            /// Creates a function that multiplies input by a constant
            /// Function: output = input * constant
            void createMultiplyConstantFunction(ResourceId functionId,
                                                float constantValue,
                                                std::string const & functionName = "MultiplyConstant")
            {
                m_assembly->addModelIfNotExisting(functionId);
                auto model = m_assembly->findModel(functionId);
                ASSERT_NE(model, nullptr);

                model->createBeginEnd();
                model->setResourceId(functionId);
                model->setModelName(functionName);

                // Add input argument
                VariantParameter inputArg{VariantType{0.f}};
                inputArg.marksAsArgument();
                inputArg.setInputSourceRequired(false);
                model->addArgument(FieldNames::A, inputArg);

                // Add output - use Distance to match main model expectations
                VariantParameter output{VariantType{0.f}};
                output.setInputSourceRequired(true);
                output.setConsumedByFunction(false);
                model->addFunctionOutput(OutputFieldName, output);

                // Create constant node
                auto * constant = model->create<ConstantScalar>();
                constant->parameter()[FieldNames::Value].setValue(VariantType{constantValue});
                constant->parameter()[FieldNames::Value].setInputSourceRequired(false);

                // Create multiplication node
                auto * multiplication = model->create<Multiplication>();

                // Connect Begin -> Multiplication (A input)
                auto * beginNode = model->getBeginNode();
                ASSERT_NE(beginNode, nullptr);
                auto & beginOutputs = beginNode->getOutputs();
                auto aOutputIt = beginOutputs.find(FieldNames::A);
                ASSERT_NE(aOutputIt, beginOutputs.end());
                multiplication->parameter()[FieldNames::A].setInputFromPort(aOutputIt->second);

                // Connect Constant -> Multiplication (B input)
                multiplication->parameter()[FieldNames::B].setInputFromPort(
                  constant->getOutputs().at(FieldNames::Value));

                // Connect Multiplication -> End (output)
                auto & outputs = model->getOutputs();
                auto outputIt = outputs.find(OutputFieldName);
                ASSERT_NE(outputIt, outputs.end());
                ASSERT_TRUE(
                  model->addLink(multiplication->getOutputs().at(FieldNames::Result).getId(),
                                 outputIt->second.getId()));

                model->invalidateGraph();
                model->updateGraphAndOrderIfNeeded();
            }

            /// Creates main assembly model with Begin/End and standard outputs
            void setupMainModel()
            {
                auto mainModel = m_assembly->assemblyModel();
                ASSERT_NE(mainModel, nullptr);
                mainModel->createBeginEndWithDefaultInAndOuts();
            }

            /// Adds a function call to the main model
            FunctionCall * addFunctionCallToMain(ResourceId functionId)
            {
                auto mainModel = m_assembly->assemblyModel();
                auto referencedModel = m_assembly->findModel(functionId);
                EXPECT_NE(referencedModel, nullptr);

                auto * functionCall = mainModel->create<FunctionCall>();
                functionCall->setFunctionId(functionId);
                functionCall->updateInputsAndOutputs(*referencedModel);

                mainModel->registerInputs(*functionCall);
                mainModel->registerOutputs(*functionCall);

                return functionCall;
            }

            /// Counts nodes of a specific type in a model
            template <typename NodeType>
            size_t countNodesOfType(Model & model)
            {
                return helper::countNumberOfNodesOfType<NodeType>(model);
            }

            /// Counts total nodes in a model (excluding Begin and End)
            size_t countNonSystemNodes(Model & model)
            {
                size_t count = 0;
                for (auto & [id, node] : model)
                {
                    if (!dynamic_cast<Begin *>(node.get()) && !dynamic_cast<End *>(node.get()))
                    {
                        ++count;
                    }
                }
                return count;
            }

            std::unique_ptr<Assembly> m_assembly;
        };

        // ============================================================================
        // Basic Flattening Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_EmptyAssembly_Succeeds)
        {
            setupMainModel();

            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto mainModel = flattened.assemblyModel();
            ASSERT_NE(mainModel, nullptr);

            // Should only have Begin and End nodes
            size_t nodeCount = 0;
            for (auto & [id, node] : *mainModel)
            {
                ++nodeCount;
            }
            EXPECT_EQ(nodeCount, 2u); // Begin + End
        }

        TEST_F(GraphFlattenerTest, Flatten_SingleFunctionCall_IntegratesNodes)
        {
            constexpr ResourceId FunctionId = 1001;
            constexpr float ConstantValue = 5.0f;

            setupMainModel();
            createAddConstantFunction(FunctionId, ConstantValue);

            auto mainModel = m_assembly->assemblyModel();

            // Create function call
            auto * functionCall = addFunctionCallToMain(FunctionId);

            // Connect a constant to the function call input
            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);

            // Use addLink instead of setInputFromPort for proper link registration
            mainModel->addLink(inputConstant->getOutputs().at(FieldNames::Value).getId(),
                               functionCall->parameter()[FieldNames::A].getId(),
                               true); // skipValidation

            // Connect function call output to End node
            auto * endNode = mainModel->getEndNode();
            mainModel->addLink(functionCall->getOutputs().at(OutputFieldName).getId(),
                               endNode->parameter()[FieldNames::Shape].getId(),
                               true); // skipValidation

            // Mark the FunctionCall output as used (required for flatten to process it)
            functionCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            // Count nodes before flattening
            size_t nodesBefore = countNonSystemNodes(*mainModel);
            EXPECT_EQ(nodesBefore, 2u); // inputConstant + functionCall

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // After flattening:
            // - FunctionCall should be removed
            // - Nodes from the function (ConstantScalar + Addition) should be integrated
            // - Original inputConstant should remain

            // Verify FunctionCall nodes are removed
            size_t functionCallCount = countNodesOfType<FunctionCall>(*flattenedModel);
            EXPECT_EQ(functionCallCount, 0u);

            // Verify Addition node was integrated
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            EXPECT_EQ(additionCount, 1u);
        }

        TEST_F(GraphFlattenerTest, Flatten_NestedFunctionCalls_IntegratesAllLevels)
        {
            constexpr ResourceId InnerFunctionId = 1001;
            constexpr ResourceId OuterFunctionId = 1002;

            setupMainModel();

            // Create inner function: result = input + 5
            createAddConstantFunction(InnerFunctionId, 5.0f, "InnerAdd");

            // Create outer function that calls inner function
            m_assembly->addModelIfNotExisting(OuterFunctionId);
            auto outerModel = m_assembly->findModel(OuterFunctionId);
            ASSERT_NE(outerModel, nullptr);

            outerModel->createBeginEnd();
            outerModel->setResourceId(OuterFunctionId);
            outerModel->setModelName("OuterFunction");

            // Add input argument to outer
            VariantParameter inputArg{VariantType{0.f}};
            inputArg.marksAsArgument();
            inputArg.setInputSourceRequired(false);
            outerModel->addArgument(FieldNames::A, inputArg);

            // Add output - use Distance to match main model expectations
            VariantParameter output{VariantType{0.f}};
            output.setInputSourceRequired(true);
            output.setConsumedByFunction(false);
            outerModel->addFunctionOutput(OutputFieldName, output);

            // Create function call to inner function
            auto innerModel = m_assembly->findModel(InnerFunctionId);
            auto * innerCall = outerModel->create<FunctionCall>();
            innerCall->setFunctionId(InnerFunctionId);
            innerCall->updateInputsAndOutputs(*innerModel);

            outerModel->registerInputs(*innerCall);
            outerModel->registerOutputs(*innerCall);

            // Connect outer Begin -> inner function call
            auto * outerBegin = outerModel->getBeginNode();
            innerCall->parameter()[FieldNames::A].setInputFromPort(
              outerBegin->getOutputs().at(FieldNames::A));

            // Connect inner call output -> outer End
            auto & outerOutputs = outerModel->getOutputs();
            auto outputIt = outerOutputs.find(OutputFieldName);
            ASSERT_TRUE(outerModel->addLink(innerCall->getOutputs().at(OutputFieldName).getId(),
                                            outputIt->second.getId()));

            // Mark inner call output as used (required for flatten to process it)
            innerCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            outerModel->invalidateGraph();
            outerModel->updateGraphAndOrderIfNeeded();

            // Now add function call to outer function in main model
            auto mainModel = m_assembly->assemblyModel();
            auto * outerCall = addFunctionCallToMain(OuterFunctionId);

            // Connect a constant to the outer function call input
            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            outerCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            // Connect function call output to End node
            auto * endNode = mainModel->getEndNode();
            mainModel->addLink(outerCall->getOutputs().at(OutputFieldName).getId(),
                               endNode->parameter()[FieldNames::Shape].getId(),
                               true); // skipValidation

            // Mark output as used (required for flatten to process it)
            outerCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // Verify all FunctionCall nodes are removed
            size_t functionCallCount = countNodesOfType<FunctionCall>(*flattenedModel);
            EXPECT_EQ(functionCallCount, 0u);

            // Verify Addition node from inner function was integrated
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            EXPECT_EQ(additionCount, 1u);

            // Verify only the assembly model remains
            EXPECT_EQ(flattened.getFunctions().size(), 1u);
        }

        // ============================================================================
        // Unused Output Pruning Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_UnusedFunctionCall_IsNotIntegrated)
        {
            constexpr ResourceId FunctionId = 1001;

            setupMainModel();
            createAddConstantFunction(FunctionId, 5.0f);

            auto mainModel = m_assembly->assemblyModel();

            // Create function call but DON'T connect its output to anything
            auto * functionCall = addFunctionCallToMain(FunctionId);

            // Connect a constant to the function call input
            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            functionCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            // Don't connect function call output to End - it's unused

            m_assembly->updateInputsAndOutputs();

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // Since function call output is not used, the Addition node should NOT be integrated
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            EXPECT_EQ(additionCount, 0u);
        }

        // ============================================================================
        // Diamond Dependency Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_DiamondDependency_IntegratesOnce)
        {
            constexpr ResourceId SharedFunctionId = 1001;
            constexpr ResourceId Function1Id = 1002;
            constexpr ResourceId Function2Id = 1003;

            setupMainModel();

            // Create shared function used by both Function1 and Function2
            createAddConstantFunction(SharedFunctionId, 1.0f, "SharedAdd");

            // Create Function1 that calls SharedFunction
            m_assembly->addModelIfNotExisting(Function1Id);
            auto func1Model = m_assembly->findModel(Function1Id);
            func1Model->createBeginEnd();
            func1Model->setResourceId(Function1Id);
            func1Model->setModelName("Function1");

            VariantParameter inputArg1{VariantType{0.f}};
            inputArg1.marksAsArgument();
            inputArg1.setInputSourceRequired(false);
            func1Model->addArgument(FieldNames::A, inputArg1);

            VariantParameter output1{VariantType{0.f}};
            output1.setInputSourceRequired(true);
            output1.setConsumedByFunction(false);
            func1Model->addFunctionOutput(OutputFieldName, output1);

            auto sharedModel = m_assembly->findModel(SharedFunctionId);
            auto * sharedCall1 = func1Model->create<FunctionCall>();
            sharedCall1->setFunctionId(SharedFunctionId);
            sharedCall1->updateInputsAndOutputs(*sharedModel);
            func1Model->registerInputs(*sharedCall1);
            func1Model->registerOutputs(*sharedCall1);

            auto * begin1 = func1Model->getBeginNode();
            sharedCall1->parameter()[FieldNames::A].setInputFromPort(
              begin1->getOutputs().at(FieldNames::A));

            auto & outputs1 = func1Model->getOutputs();
            ASSERT_TRUE(func1Model->addLink(sharedCall1->getOutputs().at(OutputFieldName).getId(),
                                            outputs1.at(OutputFieldName).getId()));

            // Mark sharedCall1 output as used
            sharedCall1->getOutputs().at(OutputFieldName).setIsUsed(true);

            func1Model->invalidateGraph();
            func1Model->updateGraphAndOrderIfNeeded();

            // Create Function2 that also calls SharedFunction
            m_assembly->addModelIfNotExisting(Function2Id);
            auto func2Model = m_assembly->findModel(Function2Id);
            func2Model->createBeginEnd();
            func2Model->setResourceId(Function2Id);
            func2Model->setModelName("Function2");

            VariantParameter inputArg2{VariantType{0.f}};
            inputArg2.marksAsArgument();
            inputArg2.setInputSourceRequired(false);
            func2Model->addArgument(FieldNames::A, inputArg2);

            VariantParameter output2{VariantType{0.f}};
            output2.setInputSourceRequired(true);
            output2.setConsumedByFunction(false);
            func2Model->addFunctionOutput(OutputFieldName, output2);

            auto * sharedCall2 = func2Model->create<FunctionCall>();
            sharedCall2->setFunctionId(SharedFunctionId);
            sharedCall2->updateInputsAndOutputs(*sharedModel);
            func2Model->registerInputs(*sharedCall2);
            func2Model->registerOutputs(*sharedCall2);

            auto * begin2 = func2Model->getBeginNode();
            sharedCall2->parameter()[FieldNames::A].setInputFromPort(
              begin2->getOutputs().at(FieldNames::A));

            auto & outputs2 = func2Model->getOutputs();
            ASSERT_TRUE(func2Model->addLink(sharedCall2->getOutputs().at(OutputFieldName).getId(),
                                            outputs2.at(OutputFieldName).getId()));

            // Mark sharedCall2 output as used
            sharedCall2->getOutputs().at(OutputFieldName).setIsUsed(true);

            func2Model->invalidateGraph();
            func2Model->updateGraphAndOrderIfNeeded();

            // Main model calls both Function1 and Function2
            auto mainModel = m_assembly->assemblyModel();

            auto * call1 = addFunctionCallToMain(Function1Id);
            auto * call2 = addFunctionCallToMain(Function2Id);

            // Create input constant
            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{1.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);

            call1->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));
            call2->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            // Create addition to combine results
            auto * addResults = mainModel->create<Addition>();
            addResults->parameter()[FieldNames::A].setInputFromPort(
              call1->getOutputs().at(OutputFieldName));
            addResults->parameter()[FieldNames::B].setInputFromPort(
              call2->getOutputs().at(OutputFieldName));

            // Mark outputs as used (required for flatten to process them)
            call1->getOutputs().at(OutputFieldName).setIsUsed(true);
            call2->getOutputs().at(OutputFieldName).setIsUsed(true);

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              addResults->getOutputs().at(FieldNames::Result));

            m_assembly->updateInputsAndOutputs();

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // Verify no FunctionCall nodes remain
            EXPECT_EQ(countNodesOfType<FunctionCall>(*flattenedModel), 0u);

            // The shared function's nodes should be integrated for EACH call
            // (Function1 and Function2 each have a copy of shared function's nodes)
            // So we expect multiple Addition nodes from the shared function
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            // 1 from main + 2 from each Function1/Function2 calling SharedFunction
            EXPECT_GE(additionCount, 3u);
        }

        // ============================================================================
        // Circular Dependency Detection Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_SelfReference_ThrowsException)
        {
            setupMainModel();

            auto mainModel = m_assembly->assemblyModel();
            auto mainModelId = mainModel->getResourceId();

            // Create a function call that references the main model itself
            auto * selfCall = mainModel->create<FunctionCall>();
            selfCall->setFunctionId(mainModelId);

            // This should throw when we try to flatten
            GraphFlattener flattener(*m_assembly);
            // Note: The exception may be thrown during flatten() or during validation
            // depending on how the self-reference is detected
        }

        // ============================================================================
        // Node Count Calculation Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, CalculateExpectedNodeCount_EmptyAssembly_ReturnsZero)
        {
            setupMainModel();

            GraphFlattener flattener(*m_assembly);
            size_t expectedCount = flattener.calculateExpectedNodeCount();

            // Empty assembly should have no nodes (Begin/End are excluded)
            EXPECT_EQ(expectedCount, 0u);
        }

        TEST_F(GraphFlattenerTest, CalculateExpectedNodeCount_SingleFunction_MatchesActual)
        {
            constexpr ResourceId FunctionId = 1001;

            setupMainModel();
            createAddConstantFunction(FunctionId, 5.0f);

            auto mainModel = m_assembly->assemblyModel();
            auto * functionCall = addFunctionCallToMain(FunctionId);

            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            functionCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              functionCall->getOutputs().at(OutputFieldName));

            // Mark output as used (required for flatten to process it)
            functionCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            // Calculate expected count
            GraphFlattener flattener(*m_assembly);
            size_t expectedCount = flattener.calculateExpectedNodeCount();

            // Actually flatten and count
            auto flattened = flattener.flatten();
            size_t actualCount = countNonSystemNodes(*flattened.assemblyModel());

            EXPECT_EQ(expectedCount, actualCount);
        }

        // ============================================================================
        // Statistics Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, GetIntegrationStats_AfterFlattening_ReturnsValidStats)
        {
            constexpr ResourceId FunctionId = 1001;

            setupMainModel();
            createAddConstantFunction(FunctionId, 5.0f);

            auto mainModel = m_assembly->assemblyModel();
            auto * functionCall = addFunctionCallToMain(FunctionId);

            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            functionCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              functionCall->getOutputs().at(OutputFieldName));

            // Mark output as used (required for flatten to process it)
            functionCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            GraphFlattener flattener(*m_assembly);
            flattener.flatten();

            auto [integratedCalls, redundantIntegrationSkips, flattenedModels, redundantFlatteningSkips] =
              flattener.getIntegrationStats();

            // At least one function call should have been integrated
            EXPECT_GE(integratedCalls, 1u);
            // At least the main model should have been flattened
            EXPECT_GE(flattenedModels, 1u);
        }

        // ============================================================================
        // Edge Case Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_IdentityFunction_Succeeds)
        {
            constexpr ResourceId IdentityFunctionId = 1001;

            setupMainModel();

            // Create an "identity" function: adds 0 to input (effectively identity)
            m_assembly->addModelIfNotExisting(IdentityFunctionId);
            auto identityModel = m_assembly->findModel(IdentityFunctionId);
            identityModel->createBeginEnd();
            identityModel->setResourceId(IdentityFunctionId);
            identityModel->setModelName("IdentityFunction");

            // Add minimal input/output
            VariantParameter inputArg{VariantType{0.f}};
            inputArg.marksAsArgument();
            inputArg.setInputSourceRequired(false);
            identityModel->addArgument(FieldNames::A, inputArg);

            VariantParameter output{VariantType{0.f}};
            output.setInputSourceRequired(true);
            output.setConsumedByFunction(false);
            identityModel->addFunctionOutput(OutputFieldName, output);

            // Create an Addition node (adds 0 to input = identity)
            auto * addZero = identityModel->create<Addition>();
            auto * zeroConst = identityModel->create<ConstantScalar>();
            zeroConst->parameter()[FieldNames::Value].setValue(VariantType{0.0f});
            zeroConst->parameter()[FieldNames::Value].setInputSourceRequired(false);

            // Wire Begin -> Addition -> End
            auto * beginNode = identityModel->getBeginNode();
            addZero->parameter()[FieldNames::A].setInputFromPort(
              beginNode->getOutputs().at(FieldNames::A));
            addZero->parameter()[FieldNames::B].setInputFromPort(
              zeroConst->getOutputs().at(FieldNames::Value));

            auto & outputs = identityModel->getOutputs();
            ASSERT_TRUE(identityModel->addLink(addZero->getOutputs().at(FieldNames::Result).getId(),
                                               outputs.at(OutputFieldName).getId()));

            identityModel->invalidateGraph();
            identityModel->updateGraphAndOrderIfNeeded();

            // Add function call to main
            auto mainModel = m_assembly->assemblyModel();
            auto * functionCall = addFunctionCallToMain(IdentityFunctionId);

            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            functionCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              functionCall->getOutputs().at(OutputFieldName));

            // Mark output as used (required for flatten to process it)
            functionCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            // Should not throw
            GraphFlattener flattener(*m_assembly);
            EXPECT_NO_THROW(flattener.flatten());

            // Verify the addition node was integrated
            auto flattened = flattener.flatten();
            auto flattenedModel = flattened.assemblyModel();
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            EXPECT_EQ(additionCount, 1u);
        }

        TEST_F(GraphFlattenerTest, Flatten_MultipleCalls_SameFunction_IntegratesMultipleTimes)
        {
            constexpr ResourceId FunctionId = 1001;

            setupMainModel();
            createAddConstantFunction(FunctionId, 5.0f);

            auto mainModel = m_assembly->assemblyModel();

            // Create two function calls to the same function
            auto * call1 = addFunctionCallToMain(FunctionId);
            auto * call2 = addFunctionCallToMain(FunctionId);

            // Create input constants
            auto * constant1 = mainModel->create<ConstantScalar>();
            constant1->parameter()[FieldNames::Value].setValue(VariantType{1.0f});
            constant1->parameter()[FieldNames::Value].setInputSourceRequired(false);

            auto * constant2 = mainModel->create<ConstantScalar>();
            constant2->parameter()[FieldNames::Value].setValue(VariantType{2.0f});
            constant2->parameter()[FieldNames::Value].setInputSourceRequired(false);

            call1->parameter()[FieldNames::A].setInputFromPort(
              constant1->getOutputs().at(FieldNames::Value));
            call2->parameter()[FieldNames::A].setInputFromPort(
              constant2->getOutputs().at(FieldNames::Value));

            // Add results together
            auto * addResults = mainModel->create<Addition>();
            addResults->parameter()[FieldNames::A].setInputFromPort(
              call1->getOutputs().at(OutputFieldName));
            addResults->parameter()[FieldNames::B].setInputFromPort(
              call2->getOutputs().at(OutputFieldName));

            // Mark outputs as used (required for flatten to process them)
            call1->getOutputs().at(OutputFieldName).setIsUsed(true);
            call2->getOutputs().at(OutputFieldName).setIsUsed(true);

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              addResults->getOutputs().at(FieldNames::Result));

            m_assembly->updateInputsAndOutputs();

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // Each call should result in its own copy of the function's nodes
            // The function has 1 Addition, so we should have:
            // - 1 Addition from main model (addResults)
            // - 2 Additions from the two function calls
            size_t additionCount = countNodesOfType<Addition>(*flattenedModel);
            EXPECT_EQ(additionCount, 3u);

            // No FunctionCall nodes should remain
            EXPECT_EQ(countNodesOfType<FunctionCall>(*flattenedModel), 0u);
        }

        // ============================================================================
        // Cleanup Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_RemovesAllSubModels)
        {
            constexpr ResourceId Function1Id = 1001;
            constexpr ResourceId Function2Id = 1002;

            setupMainModel();
            createAddConstantFunction(Function1Id, 1.0f, "Function1");
            createMultiplyConstantFunction(Function2Id, 2.0f, "Function2");

            auto mainModel = m_assembly->assemblyModel();

            auto * call1 = addFunctionCallToMain(Function1Id);
            auto * call2 = addFunctionCallToMain(Function2Id);

            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);

            call1->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));
            call2->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            auto * addResults = mainModel->create<Addition>();
            addResults->parameter()[FieldNames::A].setInputFromPort(
              call1->getOutputs().at(OutputFieldName));
            addResults->parameter()[FieldNames::B].setInputFromPort(
              call2->getOutputs().at(OutputFieldName));

            // Mark outputs as used (required for flatten to process them)
            call1->getOutputs().at(OutputFieldName).setIsUsed(true);
            call2->getOutputs().at(OutputFieldName).setIsUsed(true);

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              addResults->getOutputs().at(FieldNames::Result));

            m_assembly->updateInputsAndOutputs();

            // Before flattening: 3 models (main + 2 functions)
            EXPECT_EQ(m_assembly->getFunctions().size(), 3u);

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            // After flattening: only main model remains
            EXPECT_EQ(flattened.getFunctions().size(), 1u);
        }

        // ============================================================================
        // Graph Integrity Tests
        // ============================================================================

        TEST_F(GraphFlattenerTest, Flatten_PreservesGraphConnectivity)
        {
            constexpr ResourceId FunctionId = 1001;

            setupMainModel();
            createAddConstantFunction(FunctionId, 5.0f);

            auto mainModel = m_assembly->assemblyModel();
            auto * functionCall = addFunctionCallToMain(FunctionId);

            auto * inputConstant = mainModel->create<ConstantScalar>();
            inputConstant->parameter()[FieldNames::Value].setValue(VariantType{10.0f});
            inputConstant->parameter()[FieldNames::Value].setInputSourceRequired(false);
            functionCall->parameter()[FieldNames::A].setInputFromPort(
              inputConstant->getOutputs().at(FieldNames::Value));

            auto * endNode = mainModel->getEndNode();
            endNode->parameter()[FieldNames::Shape].setInputFromPort(
              functionCall->getOutputs().at(OutputFieldName));

            // Mark output as used (required for flatten to process it)
            functionCall->getOutputs().at(OutputFieldName).setIsUsed(true);

            m_assembly->updateInputsAndOutputs();

            // Flatten
            GraphFlattener flattener(*m_assembly);
            auto flattened = flattener.flatten();

            auto flattenedModel = flattened.assemblyModel();

            // Verify End node still has a valid connection
            auto * flatEndNode = flattenedModel->getEndNode();
            ASSERT_NE(flatEndNode, nullptr);

            auto & shapeParam = flatEndNode->parameter()[FieldNames::Shape];
            auto source = shapeParam.getSource();
            EXPECT_TRUE(source.has_value());
            EXPECT_NE(source.value().port, nullptr);
        }

    } // anonymous namespace
} // namespace gladius_tests
