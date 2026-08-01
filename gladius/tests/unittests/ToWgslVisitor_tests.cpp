#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/ToWgslVisitor.h"

#if defined(GLADIUS_ENABLE_WEBGPU)
#include "webgpu/WebGPUComputeBackend.h"
#include "webgpu/WebGPUModelSliceRequestFactory.h"
#include "webgpu/WebGPUSliceShaderComposer.h"
#endif

#include <cstdlib>
#include <gtest/gtest.h>

#include <sstream>

namespace gladius::nodes::tests
{
    TEST(ToWgslVisitor, VisitStaticAnalyticModel_EmitsEvaluateModelWithoutOpenClTokens)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);

        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);
        distance->parameter().at(FieldNames::Value).setValue(-0.25f);
        distance->parameter().at(FieldNames::Value).setModifiable(false);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();

        EXPECT_NE(shader.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"), std::string::npos);
        EXPECT_NE(shader.find("return vec4<f32>("), std::string::npos);
        EXPECT_EQ(shader.find("float3"), std::string::npos);
        EXPECT_EQ(shader.find("float16"), std::string::npos);
        EXPECT_EQ(shader.find("PAYLOAD_ARGS"), std::string::npos);
    }

    TEST(ToWgslVisitor, VisitModifiableScalar_EmitsParameterStorageReference)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);
        distance->parameter().at(FieldNames::Value).setLookUpIndex(0);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        EXPECT_NE(source.str().find("parameters.values[0u]"), std::string::npos);
        EXPECT_EQ(visitor.getRequiredParameterCount(), 1u);
        EXPECT_TRUE(visitor.usesParameter(distance->parameter().at(FieldNames::Value).getId()));
    }

    TEST(ToWgslVisitor, VisitPureMathNodes_EmitsWgslEquivalentExpressions)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * scalar = model.create<ConstantScalar>();
        auto * vectorFromScalar = model.create<VectorFromScalar>();
        auto * decomposeVector = model.create<DecomposeVector>();
        auto * arcTan2 = model.create<ArcTan2>();
        auto * fmod = model.create<Fmod>();
        auto * mod = model.create<Mod>();
        auto * arcSin = model.create<ArcSin>();
        auto * arcCos = model.create<ArcCos>();
        auto * arcTan = model.create<ArcTan>();
        auto * exp = model.create<Exp>();
        auto * log = model.create<Log>();
        auto * log2 = model.create<Log2>();
        auto * log10 = model.create<Log10>();
        auto * sinh = model.create<SinH>();
        auto * cosh = model.create<CosH>();
        auto * tanh = model.create<TanH>();
        auto * round = model.create<Round>();
        auto * ceil = model.create<Ceil>();
        auto * floor = model.create<Floor>();
        auto * sign = model.create<Sign>();
        auto * fract = model.create<Fract>();
        auto * select = model.create<Select>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(scalar, nullptr);
        ASSERT_NE(vectorFromScalar, nullptr);
        ASSERT_NE(decomposeVector, nullptr);
        ASSERT_NE(arcTan2, nullptr);
        ASSERT_NE(fmod, nullptr);
        ASSERT_NE(mod, nullptr);
        ASSERT_NE(arcSin, nullptr);
        ASSERT_NE(arcCos, nullptr);
        ASSERT_NE(arcTan, nullptr);
        ASSERT_NE(exp, nullptr);
        ASSERT_NE(log, nullptr);
        ASSERT_NE(log2, nullptr);
        ASSERT_NE(log10, nullptr);
        ASSERT_NE(sinh, nullptr);
        ASSERT_NE(cosh, nullptr);
        ASSERT_NE(tanh, nullptr);
        ASSERT_NE(round, nullptr);
        ASSERT_NE(ceil, nullptr);
        ASSERT_NE(floor, nullptr);
        ASSERT_NE(sign, nullptr);
        ASSERT_NE(fract, nullptr);
        ASSERT_NE(select, nullptr);

        for (auto * node : {static_cast<NodeBase *>(color), static_cast<NodeBase *>(scalar)})
        {
            for (auto & [name, parameter] : node->parameter())
            {
                parameter.setModifiable(false);
            }
        }
        scalar->parameter().at(FieldNames::Value).setValue(2.0f);
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);

        auto link = [&](NodeBase & source, std::string const & sourcePort, NodeBase & target, std::string const & targetParameter)
        {
            ASSERT_TRUE(model.addLink(source.getOutputs().at(sourcePort).getId(),
                                      target.parameter().at(targetParameter).getId()));
        };
        link(*scalar, FieldNames::Value, *vectorFromScalar, FieldNames::A);
        link(*vectorFromScalar, FieldNames::Result, *decomposeVector, FieldNames::A);
        link(*decomposeVector, FieldNames::X, *arcTan2, FieldNames::A);
        link(*scalar, FieldNames::Value, *arcTan2, FieldNames::B);
        link(*arcTan2, FieldNames::Result, *fmod, FieldNames::A);
        link(*scalar, FieldNames::Value, *fmod, FieldNames::B);
        link(*fmod, FieldNames::Result, *mod, FieldNames::A);
        link(*scalar, FieldNames::Value, *mod, FieldNames::B);

        NodeBase * previous = mod;
        for (auto * node : {static_cast<NodeBase *>(arcSin), static_cast<NodeBase *>(arcCos), static_cast<NodeBase *>(arcTan),
                            static_cast<NodeBase *>(exp), static_cast<NodeBase *>(log), static_cast<NodeBase *>(log2),
                            static_cast<NodeBase *>(log10), static_cast<NodeBase *>(sinh), static_cast<NodeBase *>(cosh),
                            static_cast<NodeBase *>(tanh), static_cast<NodeBase *>(round), static_cast<NodeBase *>(ceil),
                            static_cast<NodeBase *>(floor), static_cast<NodeBase *>(sign), static_cast<NodeBase *>(fract)})
        {
            link(*previous, FieldNames::Result, *node, FieldNames::A);
            previous = node;
        }

        link(*fract, FieldNames::Result, *select, FieldNames::A);
        link(*scalar, FieldNames::Value, *select, FieldNames::B);
        link(*fract, FieldNames::Result, *select, FieldNames::C);
        link(*scalar, FieldNames::Value, *select, FieldNames::D);
        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        link(*color, FieldNames::Vector, *end, FieldNames::Color);
        link(*select, FieldNames::Result, *end, FieldNames::Shape);

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();
        EXPECT_NE(shader.find("vec3<f32>("), std::string::npos);
        EXPECT_NE(shader.find(".x"), std::string::npos);
        EXPECT_NE(shader.find("atan2("), std::string::npos);
        EXPECT_NE(shader.find("trunc("), std::string::npos);
        EXPECT_NE(shader.find("floor("), std::string::npos);
        EXPECT_NE(shader.find("asin("), std::string::npos);
        EXPECT_NE(shader.find("acos("), std::string::npos);
        EXPECT_NE(shader.find("atan("), std::string::npos);
        EXPECT_NE(shader.find("exp("), std::string::npos);
        EXPECT_NE(shader.find("log10("), std::string::npos);
        EXPECT_NE(shader.find("sinh("), std::string::npos);
        EXPECT_NE(shader.find("round("), std::string::npos);
        EXPECT_NE(shader.find("fract("), std::string::npos);
        EXPECT_NE(shader.find("select("), std::string::npos);
    }

    TEST(ToWgslVisitor, VisitTransformation_WithRowMajorTranslation_EmitsColumnMajorWgslMatrix)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * transformation = model.create<Transformation>();
        auto * length = model.create<Length>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(transformation, nullptr);
        ASSERT_NE(length, nullptr);
        for (auto & [name, parameter] : color->parameter())
        {
            parameter.setModifiable(false);
        }
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);

        Matrix4x4 translation{};
        translation[0][0] = 1.0f;
        translation[1][1] = 1.0f;
        translation[2][2] = 1.0f;
        translation[3][3] = 1.0f;
        translation[0][3] = 2.0f;
        translation[1][3] = 3.0f;
        translation[2][3] = 4.0f;
        transformation->parameter().at(FieldNames::Transformation).setValue(translation);
        transformation->parameter().at(FieldNames::Transformation).setModifiable(false);

        auto * begin = model.getBeginNode();
        auto * end = model.getEndNode();
        ASSERT_NE(begin, nullptr);
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(begin->getOutputs().at(FieldNames::Pos).getId(),
                                  transformation->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(model.addLink(transformation->getOutputs().at(FieldNames::Pos).getId(),
                                  length->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(length->getOutputs().at(FieldNames::Result).getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();
        EXPECT_NE(shader.find("vec4<f32>(1f, 0f, 0f, 0f)"), std::string::npos);
        EXPECT_NE(shader.find("vec4<f32>(2f, 3f, 4f, 1f)"), std::string::npos);
        EXPECT_NE(shader.find("* vec4<f32>("), std::string::npos);
    }

    TEST(ToWgslVisitor, VisitMatrixRowsAndColumns_EmitsWgslColumnMajorConstructors)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * columns = model.create<ComposeMatrixFromColumns>();
        auto * rows = model.create<ComposeMatrixFromRows>();
        auto * transpose = model.create<Transpose>();
        auto * decompose = model.create<DecomposeMatrix>();
        auto * transposeRows = model.create<Transpose>();
        auto * decomposeRows = model.create<DecomposeMatrix>();
        auto * addition = model.create<Addition>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(columns, nullptr);
        ASSERT_NE(rows, nullptr);
        ASSERT_NE(transpose, nullptr);
        ASSERT_NE(decompose, nullptr);
        ASSERT_NE(transposeRows, nullptr);
        ASSERT_NE(decomposeRows, nullptr);
        ASSERT_NE(addition, nullptr);
        for (auto * node : {static_cast<NodeBase *>(color), static_cast<NodeBase *>(columns), static_cast<NodeBase *>(rows)})
        {
            for (auto & [name, parameter] : node->parameter())
            {
                parameter.setModifiable(false);
            }
        }

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(columns->getOutputs().at(FieldNames::Result).getId(),
                                  transpose->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(transpose->getOutputs().at(FieldNames::Matrix).getId(),
                                  decompose->parameter().at(FieldNames::Matrix).getId()));
        ASSERT_TRUE(model.addLink(decompose->getOutputs().at(FieldNames::M00).getId(),
                                  addition->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(rows->getOutputs().at(FieldNames::Result).getId(),
                                  transposeRows->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(transposeRows->getOutputs().at(FieldNames::Matrix).getId(),
                                  decomposeRows->parameter().at(FieldNames::Matrix).getId()));
        ASSERT_TRUE(model.addLink(decomposeRows->getOutputs().at(FieldNames::M00).getId(),
                                  addition->parameter().at(FieldNames::B).getId()));
        ASSERT_TRUE(model.addLink(addition->getOutputs().at(FieldNames::Result).getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();
        EXPECT_NE(shader.find("mat4x4<f32>(vec4<f32>("), std::string::npos);
        EXPECT_NE(shader.find("transpose("), std::string::npos);
        EXPECT_NE(shader.find(")[0][0]"), std::string::npos);
    }

    TEST(ToWgslVisitor, VisitBoxMinMax_EmitsCenteredBoxSdf)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * box = model.create<BoxMinMax>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(box, nullptr);
        for (auto & [name, parameter] : color->parameter())
        {
            parameter.setModifiable(false);
        }
        for (auto & [name, parameter] : box->parameter())
        {
            parameter.setModifiable(false);
        }

        auto * begin = model.getBeginNode();
        auto * end = model.getEndNode();
        ASSERT_NE(begin, nullptr);
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(begin->getOutputs().at(FieldNames::Pos).getId(),
                                  box->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(model.addLink(box->getOutputs().at(FieldNames::Shape).getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();
        EXPECT_NE(shader.find("abs("), std::string::npos);
        EXPECT_NE(shader.find("length(max("), std::string::npos);
        EXPECT_NE(shader.find("vec3<f32>(0.0f)"), std::string::npos);
    }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(WebGPUModelSliceRequestFactory, Create_WithModifiableScalar_ComposesShaderAndPacksParameters)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);
        distance->parameter().at(FieldNames::Value).setValue(-0.25f);
        distance->parameter().at(FieldNames::Value).setLookUpIndex(0);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        auto const request = webgpu::WebGPUModelSliceRequestFactory::create(model, 17u, 33u, 2.0f, 0.5f);
        EXPECT_EQ(request.width, 17u);
        EXPECT_EQ(request.height, 33u);
        EXPECT_FLOAT_EQ(request.sliceZ, 2.0f);
        EXPECT_FLOAT_EQ(request.scale, 0.5f);
        EXPECT_EQ(request.parameterValues, std::vector<float>({-0.25f}));
        EXPECT_NE(request.shaderSource.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"), std::string::npos);
        EXPECT_EQ(request.shaderSource.find("GLADIUS_MODEL_EVALUATOR"), std::string::npos);
    }

    TEST(WebGPUModelSliceRequestFactory, CreateScene_WithModifiableScalar_PreservesEvaluatorAndParameters)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);
        for (auto & [name, parameter] : color->parameter())
        {
            parameter.setModifiable(false);
        }
        distance->parameter().at(FieldNames::Value).setValue(-0.25f);
        distance->parameter().at(FieldNames::Value).setLookUpIndex(0);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        auto const snapshot = webgpu::WebGPUModelSliceRequestFactory::createScene(model, 17u);

        ASSERT_TRUE(snapshot.isValid());
        EXPECT_EQ(snapshot.sceneGeneration, 17u);
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities, compute::RendererCapability::AnalyticRendering));
        EXPECT_EQ(snapshot.parameterValues, std::vector<float>({-0.25f}));
        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"), std::string::npos);
    }

    TEST(WebGPUModelSliceRequestFactory, Create_WithModifiableMatrix_PacksRowMajorValues)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * transformation = model.create<Transformation>();
        auto * length = model.create<Length>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(transformation, nullptr);
        ASSERT_NE(length, nullptr);
        for (auto & [name, parameter] : color->parameter())
        {
            parameter.setModifiable(false);
        }

        Matrix4x4 translation{};
        translation[0][0] = 1.0f;
        translation[1][1] = 1.0f;
        translation[2][2] = 1.0f;
        translation[3][3] = 1.0f;
        translation[0][3] = 2.0f;
        translation[1][3] = 3.0f;
        translation[2][3] = 4.0f;
        transformation->parameter().at(FieldNames::Transformation).setValue(translation);
        transformation->parameter().at(FieldNames::Transformation).setLookUpIndex(0);

        auto * begin = model.getBeginNode();
        auto * end = model.getEndNode();
        ASSERT_NE(begin, nullptr);
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(begin->getOutputs().at(FieldNames::Pos).getId(),
                                  transformation->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(model.addLink(transformation->getOutputs().at(FieldNames::Pos).getId(),
                                  length->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(length->getOutputs().at(FieldNames::Result).getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        auto const request = webgpu::WebGPUModelSliceRequestFactory::create(model, 17u, 33u, 0.0f, 1.0f);
        ASSERT_EQ(request.parameterValues.size(), 16u);
        EXPECT_FLOAT_EQ(request.parameterValues[0], 1.0f);
        EXPECT_FLOAT_EQ(request.parameterValues[3], 2.0f);
        EXPECT_FLOAT_EQ(request.parameterValues[7], 3.0f);
        EXPECT_FLOAT_EQ(request.parameterValues[11], 4.0f);
        EXPECT_FLOAT_EQ(request.parameterValues[15], 1.0f);
        EXPECT_NE(request.shaderSource.find("parameters.values[0u]"), std::string::npos);
        EXPECT_NE(request.shaderSource.find("parameters.values[15u]"), std::string::npos);
    }

    TEST(WebGPUModelSliceRequestFactory, Create_WithFunctionCall_FlattensAssemblyBeforeLowering)
    {
        constexpr ResourceId HELPER_FUNCTION_ID = 100u;
        Assembly assembly;
        ASSERT_TRUE(assembly.addModelIfNotExisting(HELPER_FUNCTION_ID));
        auto helper = assembly.findModel(HELPER_FUNCTION_ID);
        ASSERT_NE(helper, nullptr);
        helper->createBeginEndWithDefaultInAndOuts();

        auto * helperDistance = helper->create<ConstantScalar>();
        ASSERT_NE(helperDistance, nullptr);
        helperDistance->parameter().at(FieldNames::Value).setValue(-0.25f);
        helperDistance->parameter().at(FieldNames::Value).setModifiable(false);
        auto * helperEnd = helper->getEndNode();
        ASSERT_NE(helperEnd, nullptr);
        ASSERT_TRUE(helper->addLink(helperDistance->getValueOutputPort().getId(),
                                    helperEnd->parameter().at(FieldNames::Shape).getId()));

        auto root = assembly.assemblyModel();
        ASSERT_NE(root, nullptr);
        root->createBeginEndWithDefaultInAndOuts();
        auto * color = root->create<ConstantVector>();
        ASSERT_NE(color, nullptr);
        for (auto & [name, parameter] : color->parameter())
        {
            parameter.setModifiable(false);
        }
        auto * functionCall = root->createFunctionCallNode(HELPER_FUNCTION_ID, *helper);
        ASSERT_NE(functionCall, nullptr);
        auto * rootBegin = root->getBeginNode();
        auto * rootEnd = root->getEndNode();
        ASSERT_NE(rootBegin, nullptr);
        ASSERT_NE(rootEnd, nullptr);
        ASSERT_TRUE(root->addLink(rootBegin->getOutputs().at(FieldNames::Pos).getId(),
                                  functionCall->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(root->addLink(functionCall->getOutputs().at(FieldNames::Shape).getId(),
                                  rootEnd->parameter().at(FieldNames::Shape).getId()));
        functionCall->getOutputs().at(FieldNames::Shape).setIsUsed(true);
        ASSERT_TRUE(root->addLink(color->getVectorOutputPort().getId(),
                                  rootEnd->parameter().at(FieldNames::Color).getId()));

        auto const request = webgpu::WebGPUModelSliceRequestFactory::create(assembly, 17u, 33u, 0.0f, 1.0f);
        EXPECT_NE(request.shaderSource.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"), std::string::npos);
        EXPECT_EQ(request.shaderSource.find("FunctionCall"), std::string::npos);
        EXPECT_TRUE(root->getNode(functionCall->getId()).has_value());
    }

    TEST(ToWgslVisitor, ComposeSliceShader_WithGeneratedEvaluator_ReplacesTemplateMarker)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);
        distance->parameter().at(FieldNames::Value).setValue(-0.25f);
        distance->parameter().at(FieldNames::Value).setModifiable(false);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                      end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        model.visitNodes(visitor);
        std::ostringstream evaluator;
        visitor.write(evaluator);

        auto const shader = webgpu::WebGPUSliceShaderComposer::compose(evaluator.str());
        EXPECT_NE(shader.find("fn evaluateModel(position: vec3<f32>) -> vec4<f32>"), std::string::npos);
        EXPECT_NE(shader.find("let model = evaluateModel(position);"), std::string::npos);
        EXPECT_EQ(shader.find("GLADIUS_MODEL_EVALUATOR"), std::string::npos);
    }

    TEST(ToWgslVisitor, ComposeSliceShader_WithGeneratedEvaluator_ExecutesOnWebGpu)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * color = model.create<ConstantVector>();
        auto * distance = model.create<ConstantScalar>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(distance, nullptr);
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);
        distance->parameter().at(FieldNames::Value).setValue(-0.25f);
        distance->parameter().at(FieldNames::Value).setLookUpIndex(0);

        auto * end = model.getEndNode();
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));
        ASSERT_TRUE(model.addLink(distance->getValueOutputPort().getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        model.visitNodes(visitor);
        std::ostringstream evaluator;
        visitor.write(evaluator);

        std::unique_ptr<webgpu::WebGPUComputeBackend> backend;
        try
        {
            backend = std::make_unique<webgpu::WebGPUComputeBackend>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        auto submission = backend->submitSlice(
          compute::SliceRequest{.width = 17u,
                                .height = 33u,
                                .sliceZ = 0.0f,
                                .scale = 1.0f,
                                .shaderSource = webgpu::WebGPUSliceShaderComposer::compose(evaluator.str()),
                                .parameterValues = {-0.25f}});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->pixels.front(), 0xFFB0753Bu);
    }

    TEST(WebGPUModelSliceRequestFactory, CreateFrame_WithAnalyticSphere_RayMarchesGeneratedModel)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        Model model;
        model.createBeginEndWithDefaultInAndOuts();
        auto * color = model.create<ConstantVector>();
        auto * radius = model.create<ConstantScalar>();
        auto * length = model.create<Length>();
        auto * sphere = model.create<Subtraction>();
        ASSERT_NE(color, nullptr);
        ASSERT_NE(radius, nullptr);
        ASSERT_NE(length, nullptr);
        ASSERT_NE(sphere, nullptr);
        for (auto * node : {static_cast<NodeBase *>(color), static_cast<NodeBase *>(radius)})
        {
            for (auto & [name, parameter] : node->parameter())
            {
                parameter.setModifiable(false);
            }
        }
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        radius->parameter().at(FieldNames::Value).setValue(0.5f);

        auto * begin = model.getBeginNode();
        auto * end = model.getEndNode();
        ASSERT_NE(begin, nullptr);
        ASSERT_NE(end, nullptr);
        ASSERT_TRUE(model.addLink(begin->getOutputs().at(FieldNames::Pos).getId(),
                                  length->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(length->getOutputs().at(FieldNames::Result).getId(),
                                  sphere->parameter().at(FieldNames::A).getId()));
        ASSERT_TRUE(model.addLink(radius->getValueOutputPort().getId(),
                                  sphere->parameter().at(FieldNames::B).getId()));
        ASSERT_TRUE(model.addLink(sphere->getOutputs().at(FieldNames::Result).getId(),
                                  end->parameter().at(FieldNames::Shape).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  end->parameter().at(FieldNames::Color).getId()));

        auto request = webgpu::WebGPUModelSliceRequestFactory::createFrame(
          model,
          compute::FrameRequest{.width = 33u,
                                .height = 33u,
                                .eyePosition = {0.0f, 0.0f, 2.0f},
                                .maxTravelDistance = 10.0f});
        EXPECT_NE(request.shaderSource.find("fn estimate_normal"), std::string::npos);
        EXPECT_NE(request.shaderSource.find("length("), std::string::npos);

        std::unique_ptr<webgpu::WebGPUComputeBackend> backend;
        try
        {
            backend = std::make_unique<webgpu::WebGPUComputeBackend>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        auto submission = backend->submitFrame(std::move(request));
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->pixels.front(), 0xFF1A1A1Au);
        EXPECT_EQ(result->pixels[(16u * 33u) + 16u], 0xFFFFDB8Cu);
    }
#endif
}
