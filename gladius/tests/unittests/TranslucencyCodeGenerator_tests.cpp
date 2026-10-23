#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/GraphFlattener.h"
#include "nodes/Model.h"
#include "nodes/OptimizeOutputs.h"
#include "nodes/TranslucencyCodeGenerator.h"

#include <gtest/gtest.h>

namespace gladius::nodes::tests
{
    namespace
    {
        /// @brief Wire a constant scalar translucency channel into a model's End node.
        void addConstantTranslucencyChannel(Model & model, float const value)
        {
            VariantParameter channel{VariantType{value}};
            channel.setInputSourceRequired(true);
            model.addFunctionOutput(FieldNames::Translucency, channel);

            auto * constant = model.create<ConstantScalar>();
            EXPECT_NE(constant, nullptr);
            if (constant == nullptr)
            {
                return;
            }
            constant->parameter().at(FieldNames::Value).setValue(VariantType{value});
            constant->parameter().at(FieldNames::Value).setModifiable(false);

            auto * endNode = model.getEndNode();
            EXPECT_TRUE(model.addLink(constant->getValueOutputPort().getId(),
                                      endNode->parameter().at(FieldNames::Translucency).getId()));
        }

        /// @brief Assembly whose single model exposes a translucency channel.
        Assembly createFlattenedTranslucentAssembly(float const value)
        {
            Assembly assembly;
            auto & assemblyModel = assembly.assemblyModel();
            assemblyModel->createBeginEndWithDefaultInAndOuts();
            addConstantTranslucencyChannel(*assemblyModel, value);
            assembly.updateInputsAndOutputs();
            return assembly;
        }
    }

    TEST(TranslucencyCodeGenerator, OpaqueStub_DefinesScalarEntryPoint)
    {
        auto const source = createOpaqueTranslucencyKernelSource();

        EXPECT_NE(source.find("float translucencyModel(float3 pos, PAYLOAD_ARGS)"),
                  std::string::npos);
        EXPECT_NE(source.find("return 0.f;"), std::string::npos);
    }

    TEST(TranslucencyCodeGenerator, ModelWithoutChannel_FallsBackToOpaqueStub)
    {
        Assembly assembly;
        assembly.assemblyModel()->createBeginEndWithDefaultInAndOuts();
        assembly.updateInputsAndOutputs();

        auto const result = createTranslucencyKernelSource(assembly);

        EXPECT_FALSE(result.active);
        EXPECT_EQ(result.source, createOpaqueTranslucencyKernelSource());
    }

    TEST(TranslucencyCodeGenerator, FlattenedTranslucentModel_EmitsScalarEntryClamped)
    {
        auto assembly = createFlattenedTranslucentAssembly(0.75f);

        auto const result = createTranslucencyKernelSource(assembly);

        EXPECT_TRUE(result.active);
        EXPECT_NE(result.source.find("float translucencyModel(float3 Input_1_pos, PAYLOAD_ARGS);"),
                  std::string::npos);
        EXPECT_NE(result.source.find("return clamp(ConstantScalar_3_value, 0.f, 1.f);"),
                  std::string::npos);
        // The shape/color entry point belongs to the other pass and must not be emitted here.
        EXPECT_EQ(result.source.find("float4 model("), std::string::npos);
    }

    TEST(TranslucencyCodeGenerator, FlattenedTranslucentModel_OmitsShapeSubgraph)
    {
        Assembly assembly;
        auto & assemblyModel = assembly.assemblyModel();
        assemblyModel->createBeginEndWithDefaultInAndOuts();
        addConstantTranslucencyChannel(*assemblyModel, 0.5f);

        auto * const shape = assemblyModel->create<ConstantScalar>();
        ASSERT_NE(shape, nullptr);
        shape->parameter().at(FieldNames::Value).setValue(VariantType{-0.25f});
        shape->parameter().at(FieldNames::Value).setModifiable(false);
        auto * const endNode = assemblyModel->getEndNode();
        ASSERT_NE(endNode, nullptr);
        ASSERT_TRUE(assemblyModel->addLink(shape->getValueOutputPort().getId(),
                                           endNode->parameter().at(FieldNames::Shape).getId()));
        assembly.updateInputsAndOutputs();

        auto const result = createTranslucencyKernelSource(assembly);

        EXPECT_TRUE(result.active);
        EXPECT_EQ(result.source.find("-0.250000"), std::string::npos);
        EXPECT_NE(result.source.find("0.500000"), std::string::npos);
    }

    TEST(TranslucencyCodeGenerator, NonFlattenedAssembly_FallsBackToOpaqueStub)
    {
        Assembly assembly;
        auto & assemblyModel = assembly.assemblyModel();
        assemblyModel->createBeginEndWithDefaultInAndOuts();

        ResourceId const functionId{1};
        ASSERT_TRUE(assembly.addModelIfNotExisting(functionId));
        auto functionModel = assembly.findModel(functionId);
        ASSERT_NE(functionModel, nullptr);
        functionModel->createBeginEndWithDefaultInAndOuts();
        functionModel->setResourceId(functionId);
        functionModel->setModelName("TranslucentPart");
        addConstantTranslucencyChannel(*functionModel, 0.5f);

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

        ASSERT_TRUE(assemblyModel->addLink(beginNode->getOutputs().at(FieldNames::Pos).getId(),
                                           call->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(assemblyModel->addLink(
          call->getOutputs().at(FieldNames::Translucency).getId(),
          endNode->parameter().at(FieldNames::Translucency).getId()));

        assembly.updateInputsAndOutputs();

        auto const result = createTranslucencyKernelSource(assembly);

        EXPECT_FALSE(result.active);
        EXPECT_EQ(result.source, createOpaqueTranslucencyKernelSource());
    }

    TEST(TranslucencyCodeGenerator, FlattenedAssembly_EntryPointMatchesKernelDeclaration)
    {
        auto assembly = createFlattenedTranslucentAssembly(0.25f);

        OptimizeOutputs optimizer{&assembly};
        optimizer.optimize();
        GraphFlattener flattener{assembly};
        auto flattened = flattener.flatten();

        auto const result = createTranslucencyKernelSource(flattened);

        EXPECT_TRUE(result.active);
        // The static kernels declare exactly this signature in rendering.h.
        EXPECT_NE(result.source.find("float translucencyModel(float3 Input_1_pos, PAYLOAD_ARGS);"),
                  std::string::npos);
    }
} // namespace gladius::nodes::tests
