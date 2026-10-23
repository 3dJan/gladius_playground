#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "compute/RenderSceneSnapshot.h"
#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/GraphFlattener.h"
#include "nodes/Model.h"
#include "nodes/ModelUtils.h"
#include "nodes/OptimizeOutputs.h"
#include "nodes/ToWgslVisitor.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace gladius::nodes::tests
{
    namespace
    {
        /// @brief Add a scalar "translucency" output channel fed by a constant to a default model.
        /// @return the constant node driving the channel, so tests can change its value.
        ConstantScalar * addConstantTranslucencyChannel(Model & model, float const value)
        {
            VariantParameter channel{VariantType{value}};
            channel.setInputSourceRequired(true);
            model.addFunctionOutput(FieldNames::Translucency, channel);

            auto * constant = model.create<ConstantScalar>();
            EXPECT_NE(constant, nullptr);
            if (constant == nullptr)
            {
                return nullptr;
            }
            constant->parameter().at(FieldNames::Value).setValue(VariantType{value});
            constant->parameter().at(FieldNames::Value).setModifiable(false);

            auto * endNode = model.getEndNode();
            EXPECT_NE(endNode, nullptr);
            if (endNode != nullptr)
            {
                EXPECT_TRUE(model.addLink(constant->getValueOutputPort().getId(),
                                          endNode->parameter().at(FieldNames::Translucency).getId()));
            }
            return constant;
        }

        std::string emitEvaluator(ToWgslVisitor & visitor, Model & model)
        {
            model.visitNodes(visitor);
            std::ostringstream out;
            visitor.write(out);
            return out.str();
        }
    }

    TEST(TranslucencyChannel, DefaultModel_HasNoTranslucencyChannel)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        EXPECT_FALSE(hasScalarOutputChannel(model, FieldNames::Translucency));
        EXPECT_FALSE(isQualifiedForTranslucency(model));
    }

    TEST(TranslucencyChannel, ScalarTranslucencyOutput_QualifiesModel)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(model, 0.5f);

        EXPECT_TRUE(hasScalarOutputChannel(model, FieldNames::Translucency));
        EXPECT_TRUE(isQualifiedForTranslucency(model));
    }

    TEST(TranslucencyChannel, VectorTranslucencyOutput_DoesNotQualifyModel)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        VariantParameter channel{VariantType{float3{0.f, 1.f, 2.f}}};
        channel.setInputSourceRequired(false);
        model.addFunctionOutput(FieldNames::Translucency, channel);

        EXPECT_TRUE(hasScalarOutputChannel(model, FieldNames::Shape));
        EXPECT_FALSE(hasScalarOutputChannel(model, FieldNames::Translucency));
        EXPECT_FALSE(isQualifiedForTranslucency(model));
    }

    TEST(TranslucencyChannel, TranslucencyEvaluator_EmitsScalarEntryPointWithClamp)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(model, 0.75f);

        ToWgslVisitor visitor;
        visitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const shader = emitEvaluator(visitor, model);

        EXPECT_NE(shader.find("fn evaluateTranslucency(position: vec3<f32>) -> f32"),
                  std::string::npos);
        EXPECT_EQ(shader.find("fn evaluateModel"), std::string::npos);
        EXPECT_NE(shader.find("return clamp(0.75f, 0.0f, 1.0f);"), std::string::npos);
        EXPECT_EQ(shader.find("vec4<f32>"), std::string::npos);
    }

    TEST(TranslucencyChannel, TranslucencyEvaluator_OmitsColorAndShapeSubgraphs)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(model, 0.25f);

        // A distinctive color subgraph that must not appear in the translucency evaluator.
        auto * color = model.create<ConstantVector>();
        ASSERT_NE(color, nullptr);
        color->parameter().at(FieldNames::X).setValue(0.125f);
        color->parameter().at(FieldNames::Y).setValue(0.375f);
        color->parameter().at(FieldNames::Z).setValue(0.625f);
        for (auto const & name : {FieldNames::X, FieldNames::Y, FieldNames::Z})
        {
            color->parameter().at(name).setModifiable(false);
        }
        auto * endNode = model.getEndNode();
        ASSERT_NE(endNode, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  endNode->parameter().at(FieldNames::Color).getId()));

        ToWgslVisitor visitor;
        visitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const shader = emitEvaluator(visitor, model);

        EXPECT_EQ(shader.find("0.125f"), std::string::npos);
        EXPECT_EQ(shader.find("0.375f"), std::string::npos);
        EXPECT_EQ(shader.find("0.625f"), std::string::npos);
        EXPECT_NE(shader.find("return clamp(0.25f, 0.0f, 1.0f);"), std::string::npos);
    }

    TEST(TranslucencyChannel, TranslucencyEvaluator_UsesPositionArgument)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        VariantParameter channel{VariantType{0.f}};
        channel.setInputSourceRequired(true);
        model.addFunctionOutput(FieldNames::Translucency, channel);

        auto * beginNode = model.getBeginNode();
        auto * length = model.create<Length>();
        auto * endNode = model.getEndNode();
        ASSERT_NE(beginNode, nullptr);
        ASSERT_NE(length, nullptr);
        ASSERT_NE(endNode, nullptr);
        ASSERT_TRUE(model.addLink(beginNode->getOutputs().at(FieldNames::Pos).getId(),
                                  length->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(length->getOutputs().at(FieldNames::Result).getId(),
                                  endNode->parameter().at(FieldNames::Translucency).getId()));

        ToWgslVisitor visitor;
        visitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const shader = emitEvaluator(visitor, model);

        EXPECT_NE(shader.find("fn evaluateTranslucency(position: vec3<f32>) -> f32"),
                  std::string::npos);
        EXPECT_NE(shader.find("length("), std::string::npos);
    }

    TEST(TranslucencyChannel, TranslucencyEvaluator_ModelWithoutChannel_EmitsNothing)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        ToWgslVisitor visitor;
        visitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const shader = emitEvaluator(visitor, model);

        EXPECT_TRUE(shader.empty());
    }

    TEST(TranslucencyChannel, ShapeAndColorEvaluator_IsUnchangedByTranslucencyChannel)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(model, 0.75f);

        ToWgslVisitor visitor;
        auto const shader = emitEvaluator(visitor, model);

        EXPECT_NE(shader.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"),
                  std::string::npos);
        EXPECT_EQ(shader.find("evaluateTranslucency"), std::string::npos);
        EXPECT_EQ(shader.find("0.75f"), std::string::npos);
    }

    TEST(TranslucencyChannel, TranslucencyEvaluator_SharesParameterStorageWithShapeEvaluator)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        // Shape reads parameter 0, translucency reads parameter 1.
        auto * shape = model.create<ConstantScalar>();
        auto * translucency = model.create<ConstantScalar>();
        ASSERT_NE(shape, nullptr);
        ASSERT_NE(translucency, nullptr);
        shape->parameter().at(FieldNames::Value).setLookUpIndex(0);
        translucency->parameter().at(FieldNames::Value).setLookUpIndex(1);

        VariantParameter channel{VariantType{0.f}};
        channel.setInputSourceRequired(true);
        model.addFunctionOutput(FieldNames::Translucency, channel);

        auto * endNode = model.getEndNode();
        ASSERT_NE(endNode, nullptr);
        endNode->parameter().at(FieldNames::Color).setModifiable(false);
        ASSERT_TRUE(model.addLink(shape->getValueOutputPort().getId(),
                                  endNode->parameter().at(FieldNames::Shape).getId()));
        ASSERT_TRUE(model.addLink(translucency->getValueOutputPort().getId(),
                                  endNode->parameter().at(FieldNames::Translucency).getId()));

        ToWgslVisitor shapeVisitor;
        auto const shapeShader = emitEvaluator(shapeVisitor, model);
        EXPECT_NE(shapeShader.find("parameters.values[0u]"), std::string::npos);
        // The shape/color evaluator emits every subgraph reachable from the End node, so it
        // also covers the translucency parameter. Both entry points share one parameter storage.
        EXPECT_EQ(shapeVisitor.getRequiredParameterCount(), 2u);

        ToWgslVisitor translucencyVisitor;
        translucencyVisitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const translucencyShader = emitEvaluator(translucencyVisitor, model);
        EXPECT_NE(translucencyShader.find("parameters.values[1u]"), std::string::npos);
        EXPECT_EQ(translucencyVisitor.getRequiredParameterCount(), 2u);
    }

    TEST(TranslucencyChannel, SceneSnapshot_WithTranslucencyChannel_PopulatesSecondEvaluator)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(model, 0.5f);

        auto const snapshot =
            compute::AnalyticRenderSceneSnapshotFactory::create(model, 7u);

        EXPECT_TRUE(snapshot.isValid());
        EXPECT_EQ(snapshot.sceneGeneration, 7u);
        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("fn evaluateModel"), std::string::npos);
        EXPECT_NE(snapshot.translucencyEvaluatorWgsl.find("fn evaluateTranslucency"),
                  std::string::npos);
    }

    TEST(TranslucencyChannel, SceneSnapshot_WithoutChannel_LeavesTranslucencyEvaluatorEmpty)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto const snapshot =
            compute::AnalyticRenderSceneSnapshotFactory::create(model, 1u);

        EXPECT_TRUE(snapshot.isValid());
        EXPECT_TRUE(snapshot.translucencyEvaluatorWgsl.empty());
    }

    TEST(TranslucencyChannel, FlatteningAssembly_KeepsTranslucencyChannelWired)
    {
        Assembly assembly;
        auto & assemblyModel = assembly.assemblyModel();
        ASSERT_NE(assemblyModel, nullptr);
        assemblyModel->createBeginEndWithDefaultInAndOuts();

        // Function model: translucency = 0.5, shape = -0.25 (exactly representable in binary).
        ResourceId const functionId{1};
        ASSERT_TRUE(assembly.addModelIfNotExisting(functionId));
        auto functionModel = assembly.findModel(functionId);
        ASSERT_NE(functionModel, nullptr);
        functionModel->createBeginEndWithDefaultInAndOuts();
        functionModel->setResourceId(functionId);
        functionModel->setModelName("TranslucentPart");
        addConstantTranslucencyChannel(*functionModel, 0.5f);

        auto * functionEnd = functionModel->getEndNode();
        ASSERT_NE(functionEnd, nullptr);
        auto * functionShape = functionModel->create<ConstantScalar>();
        ASSERT_NE(functionShape, nullptr);
        functionShape->parameter().at(FieldNames::Value).setValue(VariantType{-0.25f});
        functionShape->parameter().at(FieldNames::Value).setModifiable(false);
        ASSERT_TRUE(functionModel->addLink(functionShape->getValueOutputPort().getId(),
                                           functionEnd->parameter().at(FieldNames::Shape).getId()));

        // Assembly calls the function and wires both channels into its own End node.
        auto * call = assemblyModel->create<FunctionCall>();
        ASSERT_NE(call, nullptr);
        call->setFunctionId(functionId);
        call->updateInputsAndOutputs(*functionModel);
        assemblyModel->registerInputs(*call);
        assemblyModel->registerOutputs(*call);

        auto * beginNode = assemblyModel->getBeginNode();
        auto * endNode = assemblyModel->getEndNode();
        ASSERT_NE(beginNode, nullptr);
        ASSERT_NE(endNode, nullptr);

        VariantParameter channel{VariantType{0.f}};
        channel.setInputSourceRequired(true);
        assemblyModel->addFunctionOutput(FieldNames::Translucency, channel);

        auto const translucencyOutIt = call->getOutputs().find(FieldNames::Translucency);
        ASSERT_NE(translucencyOutIt, std::end(call->getOutputs()));
        ASSERT_TRUE(assemblyModel->addLink(beginNode->getOutputs().at(FieldNames::Pos).getId(),
                                           call->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(assemblyModel->addLink(translucencyOutIt->second.getId(),
                                           endNode->parameter().at(FieldNames::Translucency).getId()));
        ASSERT_TRUE(assemblyModel->addLink(call->getOutputs().at(FieldNames::Shape).getId(),
                                           endNode->parameter().at(FieldNames::Shape).getId()));

        assembly.updateInputsAndOutputs();

        OptimizeOutputs optimizer{&assembly};
        optimizer.optimize();

        GraphFlattener flattener{assembly};
        auto flattened = flattener.flatten();
        auto & flatModel = flattened.assemblyModel();
        ASSERT_NE(flatModel, nullptr);

        EXPECT_TRUE(isQualifiedForTranslucency(*flatModel));

        ToWgslVisitor visitor;
        visitor.setEvaluatorChannel(EvaluatorChannel::Translucency);
        auto const shader = emitEvaluator(visitor, *flatModel);
        EXPECT_NE(shader.find("fn evaluateTranslucency(position: vec3<f32>) -> f32"),
                  std::string::npos);
        EXPECT_NE(shader.find("0.5f"), std::string::npos);
        // The shape subgraph must stay out of the translucency entry point.
        EXPECT_EQ(shader.find("-0.25f"), std::string::npos);
    }
}
