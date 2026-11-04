#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/ParameterLayout.h"
#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/GraphFlattener.h"
#include "nodes/Model.h"
#include "nodes/ParameterOrigin.h"

#include <gtest/gtest.h>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace gladius::compute::tests
{
    namespace
    {
        class RuntimeParameterLayoutTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_assembly.assemblyModel()->createBeginEndWithDefaultInAndOuts();
            }

            void configureScalarFunction(nodes::ResourceId modelId, bool withArgument = false)
            {
                m_assembly.addModelIfNotExisting(modelId);
                auto model = m_assembly.findModel(modelId);
                ASSERT_NE(model, nullptr);
                model->setResourceId(modelId);
                model->createBeginEnd();
                nodes::VariantParameter output{nodes::VariantType{0.0f}};
                model->addFunctionOutput(nodes::FieldNames::Distance, output);
                if (withArgument)
                {
                    nodes::VariantParameter argument{nodes::VariantType{4.0f}};
                    argument.marksAsArgument();
                    argument.setInputSourceRequired(false);
                    model->addArgument(nodes::FieldNames::A, argument);
                    auto * sum = model->create<nodes::Addition>();
                    auto & input = model->getInputs().at(nodes::FieldNames::A);
                    ASSERT_TRUE(model->addLink(input.getId(), sum->parameter().at(nodes::FieldNames::A).getId()));
                    ASSERT_TRUE(model->addLink(input.getId(), sum->parameter().at(nodes::FieldNames::B).getId()));
                    ASSERT_TRUE(model->addLink(sum->getOutputs().at(nodes::FieldNames::Result).getId(),
                                              model->getOutputs().at(nodes::FieldNames::Distance).getId()));
                }
                else
                {
                    auto * scalar = model->create<nodes::ConstantScalar>();
                    scalar->parameter().at(nodes::FieldNames::Value).setValue(4.0f);
                    scalar->parameter().at(nodes::FieldNames::Value).setModifiable(false);
                    ASSERT_TRUE(model->addLink(scalar->getValueOutputPort().getId(),
                                              model->getOutputs().at(nodes::FieldNames::Distance).getId()));
                }
            }

            void configureDistanceFunction(nodes::ResourceId modelId)
            {
                m_assembly.addModelIfNotExisting(modelId);
                auto model = m_assembly.findModel(modelId);
                ASSERT_NE(model, nullptr);
                model->setResourceId(modelId);
                model->createBeginEnd();
                nodes::VariantParameter position{nodes::VariantType{nodes::float3{0.0f, 0.0f, 0.0f}}};
                position.marksAsArgument();
                model->addArgument(nodes::FieldNames::Pos, position);
                model->addFunctionOutput(nodes::FieldNames::Distance, nodes::VariantParameter{nodes::VariantType{0.0f}});
                auto * length = model->create<nodes::Length>();
                ASSERT_TRUE(model->addLink(model->getInputs().at(nodes::FieldNames::Pos).getId(),
                                          length->parameter().at(nodes::FieldNames::A).getId()));
                ASSERT_TRUE(model->addLink(length->getOutputs().at(nodes::FieldNames::Result).getId(),
                                          model->getOutputs().at(nodes::FieldNames::Distance).getId()));
            }

            void expectOnlyOriginUpdated(RenderSceneSnapshot & snapshot, nodes::ParameterOrigin origin,
                                         std::size_t expectedCount, float originalValue, float newValue)
            {
                ASSERT_NE(snapshot.parameterLayout, nullptr);
                auto const destinations = snapshot.parameterLayout->getDestinations(origin);
                ASSERT_EQ(destinations.size(), expectedCount);
                std::set<std::size_t> offsets;
                for (auto const & destination : destinations)
                {
                    ASSERT_EQ(destination.type, nodes::ParameterTypeIndex::Float);
                    ASSERT_LT(destination.offset, snapshot.parameterValues.size());
                    EXPECT_FLOAT_EQ(snapshot.parameterValues[destination.offset], originalValue);
                    EXPECT_TRUE(offsets.insert(destination.offset).second);
                }
                auto const before = snapshot.parameterValues;
                auto const wgsl = snapshot.analyticEvaluatorWgsl;
                ASSERT_TRUE(snapshot.parameterLayout->apply(origin, nodes::VariantType{newValue},
                                                           snapshot.sceneGeneration, snapshot.parameterValues));
                for (std::size_t index = 0; index < before.size(); ++index)
                {
                    EXPECT_FLOAT_EQ(snapshot.parameterValues[index], offsets.contains(index) ? newValue : before[index]);
                }
                EXPECT_EQ(snapshot.analyticEvaluatorWgsl, wgsl);
            }

            nodes::Assembly m_assembly;
        };
    }

    TEST(ParameterLayout, Apply_RepeatedOrigin_UpdatesEveryDestinationWithoutChangingLayout)
    {
        ParameterLayout layout{7u};
        nodes::ParameterOrigin const origin{100u, 20001};
        layout.add(origin, 0u, nodes::ParameterTypeIndex::Float);
        layout.add(origin, 3u, nodes::ParameterTypeIndex::Float);
        std::vector<float> values{1.0f, 2.0f, 3.0f, 1.0f};

        EXPECT_TRUE(layout.apply(origin, nodes::VariantType{9.0f}, 7u, values));

        EXPECT_EQ(values, (std::vector<float>{9.0f, 2.0f, 3.0f, 9.0f}));
        EXPECT_EQ(layout.getDestinations(origin).size(), 2u);
    }

    TEST(ParameterLayout, Apply_IncompatibleGenerationOrType_DoesNotModifyValues)
    {
        ParameterLayout layout{7u};
        nodes::ParameterOrigin const origin{100u, 20001};
        layout.add(origin, 0u, nodes::ParameterTypeIndex::Float);
        std::vector<float> values{1.0f};

        EXPECT_FALSE(layout.apply(origin, nodes::VariantType{9.0f}, 8u, values));
        EXPECT_FALSE(layout.apply(origin, nodes::VariantType{nodes::float3{1.0f, 2.0f, 3.0f}}, 7u, values));
        EXPECT_FALSE(layout.apply(origin, nodes::VariantType{std::numeric_limits<float>::infinity()}, 7u, values));
        EXPECT_FALSE(layout.apply({101u, 20001}, nodes::VariantType{9.0f}, 7u, values));
        EXPECT_EQ(values, std::vector<float>{1.0f});
    }

    TEST(ParameterLayout, Apply_TruncatedFanOut_DoesNotPartiallyWrite)
    {
        ParameterLayout layout{7u};
        nodes::ParameterOrigin const origin{100u, 20001};
        layout.add(origin, 0u, nodes::ParameterTypeIndex::Float);
        layout.add(origin, 3u, nodes::ParameterTypeIndex::Float);
        std::vector<float> values{1.0f};

        EXPECT_FALSE(layout.apply(origin, nodes::VariantType{9.0f}, 7u, values));
        EXPECT_EQ(values, std::vector<float>{1.0f});
    }

    TEST(ParameterLayout, Apply_Matrix_PreservesExistingComponentOrder)
    {
        ParameterLayout layout{7u};
        nodes::ParameterOrigin const origin{100u, 20001};
        layout.add(origin, 0u, nodes::ParameterTypeIndex::Matrix4);
        nodes::Matrix4x4 matrix{};
        matrix[3][0] = 2.0f;
        matrix[3][1] = 3.0f;
        matrix[3][2] = 4.0f;
        std::vector<float> values(16u);

        ASSERT_TRUE(layout.apply(origin, nodes::VariantType{matrix}, 7u, values));
        EXPECT_FLOAT_EQ(values[12], 2.0f);
        EXPECT_FLOAT_EQ(values[13], 3.0f);
        EXPECT_FLOAT_EQ(values[14], 4.0f);
    }

    TEST(AnalyticRenderSceneSnapshotFactory, CreateRuntime_FirstLiteralEdit_ChangesOnlyParameterValues)
    {
        nodes::Assembly assembly;
        auto model = assembly.assemblyModel();
        model->createBeginEndWithDefaultInAndOuts();
        auto * scalar = model->create<nodes::ConstantScalar>();
        auto & parameter = scalar->parameter().at(nodes::FieldNames::Value);
        parameter.setValue(-0.25f);
        parameter.setModifiable(false);
        nodes::ParameterOrigin const origin{model->getResourceId(), parameter.getId()};
        ASSERT_TRUE(model->addLink(scalar->getValueOutputPort().getId(),
                                  model->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(assembly, 7u);
        auto const originalWgsl = snapshot.analyticEvaluatorWgsl;
        auto const layout = snapshot.parameterLayout;

        ASSERT_NE(layout, nullptr);
        for (int edit = 0; edit < 100; ++edit)
        {
            ASSERT_TRUE(layout->apply(origin, nodes::VariantType{static_cast<float>(edit)},
                                      snapshot.sceneGeneration, snapshot.parameterValues));
        }

        EXPECT_EQ(snapshot.analyticEvaluatorWgsl, originalWgsl);
        EXPECT_EQ(snapshot.parameterLayout, layout);
        EXPECT_EQ(snapshot.sceneGeneration, 7u);
        EXPECT_FALSE(parameter.isModifiable());
        EXPECT_FALSE(parameter.getOrigin().has_value());
        auto const destinations = layout->getDestinations(origin);
        ASSERT_EQ(destinations.size(), 1u);
        EXPECT_FLOAT_EQ(snapshot.parameterValues[destinations.front().offset], 99.0f);
    }

    TEST(AnalyticRenderSceneSnapshotFactory,
         CreateRuntimeFromFlat_LiteralEdit_ChangesOnlyParameterValues)
    {
        nodes::Assembly assembly;
        auto model = assembly.assemblyModel();
        model->createBeginEndWithDefaultInAndOuts();
        auto * scalar = model->create<nodes::ConstantScalar>();
        auto & parameter = scalar->parameter().at(nodes::FieldNames::Value);
        parameter.setValue(-0.25f);
        parameter.setModifiable(false);
        nodes::ParameterOrigin const origin{model->getResourceId(), parameter.getId()};
        parameter.setOrigin(origin);
        ASSERT_TRUE(model->addLink(scalar->getValueOutputPort().getId(),
                                  model->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));

        nodes::GraphFlattener flattener{assembly};
        auto const flattened = flattener.flatten();
        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntimeFromFlat(flattened, 9u);
        auto const originalWgsl = snapshot.analyticEvaluatorWgsl;

        ASSERT_TRUE(snapshot.isValid());
        ASSERT_NE(snapshot.parameterLayout, nullptr);
        auto const destinations = snapshot.parameterLayout->getDestinations(origin);
        ASSERT_EQ(destinations.size(), 1u);
        EXPECT_FLOAT_EQ(snapshot.parameterValues[destinations.front().offset], -0.25f);
        ASSERT_TRUE(snapshot.parameterLayout->apply(origin,
                                                    nodes::VariantType{0.75f},
                                                    snapshot.sceneGeneration,
                                                    snapshot.parameterValues));
        EXPECT_FLOAT_EQ(snapshot.parameterValues[destinations.front().offset], 0.75f);
        EXPECT_EQ(snapshot.analyticEvaluatorWgsl, originalWgsl);
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_RepeatedCallsWithCollidingLocalIds_SeparatesModelOrigins)
    {
        configureScalarFunction(1001u);
        ASSERT_FALSE(HasFatalFailure());
        configureScalarFunction(1002u);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto first = m_assembly.findModel(1001u);
        auto second = m_assembly.findModel(1002u);
        auto & firstLiteral = first->getOutputs().at(nodes::FieldNames::Distance).getSource()->port->getParent()->
          parameter().at(nodes::FieldNames::Value);
        auto & secondLiteral = second->getOutputs().at(nodes::FieldNames::Distance).getSource()->port->getParent()->
          parameter().at(nodes::FieldNames::Value);
        ASSERT_EQ(firstLiteral.getId(), secondLiteral.getId());
        nodes::ParameterOrigin const firstOrigin{first->getResourceId(), firstLiteral.getId()};
        nodes::ParameterOrigin const secondOrigin{second->getResourceId(), secondLiteral.getId()};
        auto * pairSum = main->create<nodes::Addition>();
        for (auto const & [model, field] : {std::pair{first, nodes::FieldNames::A}, std::pair{second, nodes::FieldNames::B}})
        {
            auto * callA = main->createFunctionCallNode(model->getResourceId(), *model);
            auto * callB = main->createFunctionCallNode(model->getResourceId(), *model);
            auto * sum = main->create<nodes::Addition>();
            ASSERT_TRUE(main->addLink(callA->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                      sum->parameter().at(nodes::FieldNames::A).getId()));
            ASSERT_TRUE(main->addLink(callB->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                      sum->parameter().at(nodes::FieldNames::B).getId()));
            ASSERT_TRUE(main->addLink(sum->getOutputs().at(nodes::FieldNames::Result).getId(),
                                      pairSum->parameter().at(field).getId()));
            callA->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
            callB->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        }
        ASSERT_TRUE(main->addLink(pairSum->getOutputs().at(nodes::FieldNames::Result).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        m_assembly.updateInputsAndOutputs();

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 17u);

        expectOnlyOriginUpdated(snapshot, firstOrigin, 2u, 4.0f, 11.0f);
        expectOnlyOriginUpdated(snapshot, secondOrigin, 2u, 4.0f, -3.0f);
        EXPECT_EQ(m_assembly.getFunctions().size(), 3u);
        EXPECT_FLOAT_EQ(std::get<float>(firstLiteral.getValue()), 4.0f);
        EXPECT_FLOAT_EQ(std::get<float>(secondLiteral.getValue()), 4.0f);
        EXPECT_FALSE(firstLiteral.getOrigin().has_value());
        EXPECT_FALSE(secondLiteral.getOrigin().has_value());
    }

    TEST_F(RuntimeParameterLayoutTest, Flatten_OptionalUnconnectedCallArgument_MaterializesValueFlagsAndCallOrigin)
    {
        configureScalarFunction(1001u, true);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto function = m_assembly.findModel(1001u);
        auto * call = main->createFunctionCallNode(1001u, *function);
        auto & argument = call->parameter().at(nodes::FieldNames::A);
        argument.setValue(4.0f);
        argument.setInputSourceRequired(false);
        argument.setModifiable(false);
        nodes::ParameterOrigin const origin{main->getResourceId(), argument.getId()};
        argument.setOrigin(origin);
        ASSERT_TRUE(main->addLink(call->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        call->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        m_assembly.updateInputsAndOutputs();

        nodes::GraphFlattener flattener{m_assembly};
        auto flattened = flattener.flatten();

        std::size_t materialized = 0u;
        for (auto const & [nodeId, node] : *flattened.assemblyModel())
        {
            EXPECT_EQ(dynamic_cast<nodes::FunctionCall *>(node.get()), nullptr);
            if (auto * sum = dynamic_cast<nodes::Addition *>(node.get()))
            {
                for (auto const & [name, parameter] : sum->parameter())
                {
                    EXPECT_EQ(parameter.getOrigin(), std::optional{origin});
                    EXPECT_FALSE(parameter.getConstSource().has_value());
                    EXPECT_FALSE(parameter.isInputSourceRequired());
                    EXPECT_FALSE(parameter.isModifiable());
                    EXPECT_FLOAT_EQ(std::get<float>(parameter.getValue()), 4.0f);
                    ++materialized;
                }
            }
        }
        EXPECT_EQ(materialized, 2u);
        EXPECT_FALSE(function->getBeginNode()->parameter().at(nodes::FieldNames::A).getOrigin().has_value());
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_EqualOptionalArgumentsOnRepeatedCalls_UsesEachCallOrigin)
    {
        configureScalarFunction(1001u, true);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto function = m_assembly.findModel(1001u);
        auto * callA = main->createFunctionCallNode(1001u, *function);
        auto * callB = main->createFunctionCallNode(1001u, *function);
        auto * sum = main->create<nodes::Addition>();
        for (auto const & [call, field] : {std::pair{callA, nodes::FieldNames::A}, std::pair{callB, nodes::FieldNames::B}})
        {
            auto & argument = call->parameter().at(nodes::FieldNames::A);
            /// Default call values are stored on the call, not inherited by updateInputsAndOutputs.
            argument.setValue(function->getBeginNode()->parameter().at(nodes::FieldNames::A).getValue());
            argument.setInputSourceRequired(false);
            argument.setModifiable(false);
            ASSERT_TRUE(main->addLink(call->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                      sum->parameter().at(field).getId()));
            call->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        }
        nodes::ParameterOrigin const originA{main->getResourceId(), callA->parameter().at(nodes::FieldNames::A).getId()};
        nodes::ParameterOrigin const originB{main->getResourceId(), callB->parameter().at(nodes::FieldNames::A).getId()};
        ASSERT_NE(originA, originB);
        ASSERT_TRUE(main->addLink(sum->getOutputs().at(nodes::FieldNames::Result).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        m_assembly.updateInputsAndOutputs();

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 18u);

        expectOnlyOriginUpdated(snapshot, originA, 2u, 4.0f, 8.0f);
        expectOnlyOriginUpdated(snapshot, originB, 2u, 4.0f, 12.0f);
        nodes::ParameterOrigin const definitionOrigin{function->getResourceId(),
          function->getBeginNode()->parameter().at(nodes::FieldNames::A).getId()};
        ASSERT_NE(snapshot.parameterLayout, nullptr);
        EXPECT_TRUE(snapshot.parameterLayout->getDestinations(definitionOrigin).empty());
        EXPECT_FALSE(callA->parameter().at(nodes::FieldNames::A).getOrigin().has_value());
        EXPECT_FALSE(callB->parameter().at(nodes::FieldNames::A).getOrigin().has_value());
    }

    TEST_F(RuntimeParameterLayoutTest, Flatten_RequiredUnconnectedCallArgument_RejectsEvenWithStoredLiteral)
    {
        configureScalarFunction(1001u, true);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto * call = main->createFunctionCallNode(1001u, *m_assembly.findModel(1001u));
        auto & argument = call->parameter().at(nodes::FieldNames::A);
        argument.setValue(4.0f);
        argument.setInputSourceRequired(true);
        ASSERT_TRUE(main->addLink(call->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        call->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        m_assembly.updateInputsAndOutputs();
        ASSERT_FALSE(argument.getConstSource().has_value());
        ASSERT_TRUE(argument.isInputSourceRequired());

        nodes::GraphFlattener flattener{m_assembly};

        EXPECT_THROW(flattener.flatten(), std::exception);
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_OptionalPassthroughArgument_PreservesCallOrigin)
    {
        configureScalarFunction(1001u, true);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto function = m_assembly.findModel(1001u);
        ASSERT_TRUE(function->addLink(function->getInputs().at(nodes::FieldNames::A).getId(),
                                      function->getOutputs().at(nodes::FieldNames::Distance).getId()));
        auto * call = main->createFunctionCallNode(1001u, *function);
        auto & argument = call->parameter().at(nodes::FieldNames::A);
        argument.setValue(4.0f);
        argument.setInputSourceRequired(false);
        nodes::ParameterOrigin const origin{main->getResourceId(), argument.getId()};
        ASSERT_TRUE(main->addLink(call->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        call->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        m_assembly.updateInputsAndOutputs();

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 21u);

        expectOnlyOriginUpdated(snapshot, origin, 1u, 4.0f, 8.0f);
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_NestedOptionalPassthrough_PreservesInnerCallOrigin)
    {
        configureScalarFunction(1001u, true);
        ASSERT_FALSE(HasFatalFailure());
        auto inner = m_assembly.findModel(1001u);
        ASSERT_TRUE(inner->addLink(inner->getInputs().at(nodes::FieldNames::A).getId(),
                                  inner->getOutputs().at(nodes::FieldNames::Distance).getId()));
        configureScalarFunction(1002u);
        ASSERT_FALSE(HasFatalFailure());
        auto outer = m_assembly.findModel(1002u);
        auto * innerCall = outer->createFunctionCallNode(1001u, *inner);
        auto & argument = innerCall->parameter().at(nodes::FieldNames::A);
        argument.setValue(4.0f);
        argument.setInputSourceRequired(false);
        nodes::ParameterOrigin const origin{outer->getResourceId(), argument.getId()};
        ASSERT_TRUE(outer->addLink(innerCall->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                  outer->getOutputs().at(nodes::FieldNames::Distance).getId()));
        innerCall->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        auto main = m_assembly.assemblyModel();
        auto * outerCall = main->createFunctionCallNode(1002u, *outer);
        ASSERT_TRUE(main->addLink(outerCall->getOutputs().at(nodes::FieldNames::Distance).getId(),
                                 main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        outerCall->getOutputs().at(nodes::FieldNames::Distance).setIsUsed(true);
        m_assembly.updateInputsAndOutputs();

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 22u);

        expectOnlyOriginUpdated(snapshot, origin, 1u, 4.0f, 8.0f);
        EXPECT_FALSE(argument.getOrigin().has_value());
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_RepeatedGradients_PreservesIndependentStepSizeOrigins)
    {
        configureDistanceFunction(1001u);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto function = m_assembly.findModel(1001u);
        auto * gradientA = main->create<nodes::FunctionGradient>();
        auto * gradientB = main->create<nodes::FunctionGradient>();
        auto * sum = main->create<nodes::Addition>();
        for (auto const & [gradient, field] : {std::pair{gradientA, nodes::FieldNames::A}, std::pair{gradientB, nodes::FieldNames::B}})
        {
            gradient->setFunctionId(1001u);
            gradient->updateInputsAndOutputs(*function);
            gradient->setSelectedScalarOutput(nodes::FieldNames::Distance);
            gradient->setSelectedVectorInput(nodes::FieldNames::Pos);
            gradient->setStepSize(0.01f);
            gradient->parameter().at(nodes::FieldNames::StepSize).setModifiable(false);
            main->registerInputs(*gradient);
            main->registerOutputs(*gradient);
            ASSERT_TRUE(main->addLink(main->getInputs().at(nodes::FieldNames::Pos).getId(),
                                      gradient->parameter().at(nodes::FieldNames::Pos).getId()));
            ASSERT_TRUE(main->addLink(gradient->getOutputs().at(nodes::FieldNames::NormalizedGradient).getId(),
                                      sum->parameter().at(field).getId()));
            gradient->getOutputs().at(nodes::FieldNames::NormalizedGradient).setIsUsed(true);
        }
        ASSERT_TRUE(main->addLink(sum->getOutputs().at(nodes::FieldNames::Result).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Color).getId()));
        m_assembly.updateInputsAndOutputs();
        nodes::ParameterOrigin const originA{main->getResourceId(), gradientA->parameter().at(nodes::FieldNames::StepSize).getId()};
        nodes::ParameterOrigin const originB{main->getResourceId(), gradientB->parameter().at(nodes::FieldNames::StepSize).getId()};

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 19u);

        expectOnlyOriginUpdated(snapshot, originA, 1u, 0.01f, 0.02f);
        expectOnlyOriginUpdated(snapshot, originB, 1u, 0.01f, 0.03f);
        EXPECT_EQ(m_assembly.getFunctions().size(), 2u);
        EXPECT_FLOAT_EQ(gradientA->getStepSize(), 0.01f);
        EXPECT_FLOAT_EQ(gradientB->getStepSize(), 0.01f);
        EXPECT_FALSE(gradientA->parameter().at(nodes::FieldNames::StepSize).getOrigin().has_value());
        EXPECT_FALSE(gradientB->parameter().at(nodes::FieldNames::StepSize).getOrigin().has_value());
    }

    TEST_F(RuntimeParameterLayoutTest, CreateRuntime_Normalization_PreservesStepSizeThroughBothLowerers)
    {
        configureDistanceFunction(1001u);
        ASSERT_FALSE(HasFatalFailure());
        auto main = m_assembly.assemblyModel();
        auto * normalize = main->create<nodes::NormalizeDistanceField>();
        normalize->setFunctionId(1001u);
        normalize->updateInputsAndOutputs(*m_assembly.findModel(1001u));
        normalize->setSelectedScalarOutput(nodes::FieldNames::Distance);
        normalize->setSelectedVectorInput(nodes::FieldNames::Pos);
        normalize->setStepSize(0.01f);
        normalize->parameter().at(nodes::FieldNames::StepSize).setModifiable(false);
        main->registerInputs(*normalize);
        main->registerOutputs(*normalize);
        ASSERT_TRUE(main->addLink(main->getInputs().at(nodes::FieldNames::Pos).getId(),
                                  normalize->parameter().at(nodes::FieldNames::Pos).getId()));
        ASSERT_TRUE(main->addLink(normalize->getOutputs().at(nodes::FieldNames::Result).getId(),
                                  main->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
        m_assembly.updateInputsAndOutputs();
        nodes::ParameterOrigin const origin{main->getResourceId(), normalize->parameter().at(nodes::FieldNames::StepSize).getId()};

        auto snapshot = AnalyticRenderSceneSnapshotFactory::createRuntime(m_assembly, 20u);

        expectOnlyOriginUpdated(snapshot, origin, 1u, 0.01f, 0.02f);
        EXPECT_EQ(m_assembly.getFunctions().size(), 2u);
        EXPECT_FLOAT_EQ(normalize->getStepSize(), 0.01f);
        EXPECT_FALSE(normalize->parameter().at(nodes::FieldNames::StepSize).getOrigin().has_value());
    }
}