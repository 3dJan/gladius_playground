/// @file TranslucencyRendering_tests.cpp
/// @brief End-to-end OpenCL check that the translucency channel attenuates interior travel.
///
/// The scene is two boxes stacked along the view axis. The opaque path stops at the near box,
/// so the far box is hidden. With RF_ENABLE_TRANSLUCENCY the compositor keeps marching, and the
/// far box only contributes once the optical depth accumulated inside the near box is accounted
/// for - which is what makes the centre pixel brighter.

#include "compute/RenderSessionInputs.h"
#include "compute/TranslucencyConfig.h"
#include "opencl_test_helper.h"

#include <compute/ComputeCore.h>
#include <nodes/Assembly.h>
#include <nodes/DerivedNodes.h>
#include <nodes/Model.h>

#include <gtest/gtest.h>

#include <cmath>

namespace gladius::tests
{
    namespace
    {
        using namespace gladius;
        using namespace gladius::nodes;

        constexpr size_t IMAGE_SIZE = 15u;
        constexpr float ALPHA = 0.1f;
        constexpr float REFERENCE_THICKNESS_MM = 1.f;

        /// @brief Add an axis-aligned box to the model and return its shape output port.
        Port & addBox(Model & model, Port & posPort, float3 const minCorner, float3 const maxCorner)
        {
            auto * minNode = model.create<ConstantVector>();
            minNode->parameter()[FieldNames::X].setValue(VariantType{minCorner.x});
            minNode->parameter()[FieldNames::Y].setValue(VariantType{minCorner.y});
            minNode->parameter()[FieldNames::Z].setValue(VariantType{minCorner.z});

            auto * maxNode = model.create<ConstantVector>();
            maxNode->parameter()[FieldNames::X].setValue(VariantType{maxCorner.x});
            maxNode->parameter()[FieldNames::Y].setValue(VariantType{maxCorner.y});
            maxNode->parameter()[FieldNames::Z].setValue(VariantType{maxCorner.z});

            auto * box = model.create<BoxMinMax>();
            EXPECT_TRUE(model.addLink(posPort.getId(), box->parameter()[FieldNames::Pos].getId()));
            EXPECT_TRUE(model.addLink(minNode->getOutputs().at(FieldNames::Vector).getId(),
                                      box->parameter()[FieldNames::Min].getId()));
            EXPECT_TRUE(model.addLink(maxNode->getOutputs().at(FieldNames::Vector).getId(),
                                      box->parameter()[FieldNames::Max].getId()));

            return box->getOutputs().at(FieldNames::Shape);
        }

        /**
         * @brief Two boxes along -Z, the near one translucent with a constant channel.
         * @param model model that already has the default begin/end
         */
        void buildStackedBoxesWithTranslucency(Model & model)
        {
            auto & posPort = model.getInputs().at(FieldNames::Pos);
            auto & nearBox = addBox(model, posPort, {-5.f, -5.f, 5.f}, {5.f, 5.f, 10.f});
            auto & farBox = addBox(model, posPort, {-5.f, -5.f, -10.f}, {5.f, 5.f, -5.f});

            // For SDFs the minimum is the union, so both boxes stay visible to the marcher.
            auto * unite = model.create<Min>();
            unite->setInputA(nearBox);
            unite->setInputB(farBox);
            EXPECT_TRUE(model.addLink(unite->getResultOutputPort().getId(),
                                      model.getEndNode()->parameter()
                                        .at(FieldNames::Shape)
                                        .getId()));

            VariantParameter channel{VariantType{ALPHA}};
            channel.setInputSourceRequired(true);
            model.addFunctionOutput(FieldNames::Translucency, channel);

            auto * alpha = model.create<ConstantScalar>();
            alpha->parameter().at(FieldNames::Value).setValue(VariantType{ALPHA});
            alpha->parameter().at(FieldNames::Value).setModifiable(false);
            EXPECT_TRUE(model.addLink(alpha->getValueOutputPort().getId(),
                                      model.getEndNode()->parameter()
                                        .at(FieldNames::Translucency)
                                        .getId()));

            model.updateGraphAndOrderIfNeeded();
        }

        /// @brief Sum of the centre pixel's channels - enough to compare brightness.
        float luminanceOfCentrePixel(ImageRGBA & image)
        {
            EXPECT_NO_THROW(image.read());
            auto const & pixels = image.getData();
            EXPECT_EQ(pixels.size(), IMAGE_SIZE * IMAGE_SIZE);
            if (pixels.empty())
            {
                return 0.f;
            }

            // Odd image size, so the middle pixel samples screenPos == (0, 0) exactly.
            auto const centre = (IMAGE_SIZE / 2u) * IMAGE_SIZE + (IMAGE_SIZE / 2u);
            auto const & pixel = pixels[centre];
            return pixel.s[0] + pixel.s[1] + pixel.s[2];
        }
    }

    TEST(TranslucencyRendering, RefreshProgram_TranslucentModel_PublishesActiveEntryPoint)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        ASSERT_TRUE(context->isValid());
        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);

        auto assembly = std::make_shared<Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();
        buildStackedBoxesWithTranslucency(*assembly->assemblyModel());

        core->setCodeGenerator(CodeGenerator::Code);
        core->refreshProgram(assembly);

        // The render kernels always call translucencyModel(), so a dispatch may only set
        // RF_ENABLE_TRANSLUCENCY when the published source really answers the channel.
        EXPECT_TRUE(core->getResourceContext()->isTranslucencyEntryPointActive());
    }

    TEST(TranslucencyRendering, RefreshProgram_OpaqueModel_PublishesInactiveEntryPoint)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        ASSERT_TRUE(context->isValid());
        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);

        auto assembly = std::make_shared<Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();
        auto & posPort = assembly->assemblyModel()->getInputs().at(FieldNames::Pos);
        auto & shapeSink = assembly->assemblyModel()->getEndNode()->parameter().at(FieldNames::Shape);
        auto & box = addBox(*assembly->assemblyModel(), posPort, {-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f});
        EXPECT_TRUE(assembly->assemblyModel()->addLink(box.getId(), shapeSink.getId()));
        assembly->assemblyModel()->updateGraphAndOrderIfNeeded();

        core->setCodeGenerator(CodeGenerator::Code);
        core->refreshProgram(assembly);

        // Such a source only gets the zero stub, whose sigma would make the model invisible.
        EXPECT_FALSE(core->getResourceContext()->isTranslucencyEntryPointActive());
    }

    TEST(TranslucencyRendering, RenderScene_TranslucentNearBox_RevealsBoxBehindIt)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        ASSERT_TRUE(context->isValid());
        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);

        auto assembly = std::make_shared<Assembly>();
        assembly->assemblyModel()->createBeginEndWithDefaultInAndOuts();
        buildStackedBoxesWithTranslucency(*assembly->assemblyModel());

        core->setCodeGenerator(CodeGenerator::Code);
        // Unconnected node parameters are read from the GPU parameter buffer, so it has to be
        // filled before codegen assigns the lookup indices - same order Document uses.
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));
        core->refreshProgram(assembly);
        ASSERT_NO_THROW(core->recompileBlockingNoLock());
        auto program = core->getBestRenderProgram();
        ASSERT_NE(program.get(), nullptr);

        auto primitives = core->getPrimitives();
        ASSERT_NE(primitives.get(), nullptr);
        ASSERT_NO_THROW(primitives->write());

        // Straight down -Z from in front of both boxes, helper geometry switched off.
        RenderSessionInputs inputs;
        inputs.eyePosition = {0.f, 0.f, 20.f};
        inputs.modelViewPerspectiveMat = {};
        inputs.modelViewPerspectiveMat.s0 = 1.f;
        inputs.modelViewPerspectiveMat.s5 = 1.f;
        inputs.modelViewPerspectiveMat.sa = -1.f;
        inputs.settings = {};
        inputs.settings.z_mm = 1000.f;
        inputs.settings.quality = 1.f;
        inputs.settings.normalOffset = 0.0001f;
        inputs.settings.approximation = AM_FULL_MODEL;
        inputs.settings.flags = 0;

        compute::TranslucencyConfig config;
        config.enabled = true;
        config.referenceThicknessMm = REFERENCE_THICKNESS_MM;
        config.interiorStepSizeMm = 0.1f;
        config.transmittanceFloor = 0.01f;
        config.maxInteriorSteps = 512.f;
        config.maxSurfaces = 3.f;

        ImageRGBA opaqueImage(*context, IMAGE_SIZE, IMAGE_SIZE);
        ASSERT_NO_THROW(opaqueImage.allocateOnDevice());
        compute::applyTranslucency(inputs.settings, {.enabled = false});
        program->renderScene(context->GetQueue(), *primitives, opaqueImage, inputs, 0u, IMAGE_SIZE);
        auto const opaqueLuminance = luminanceOfCentrePixel(opaqueImage);
        ASSERT_GT(opaqueLuminance, 0.f) << "the opaque path must show the near box";

        ImageRGBA translucentImage(*context, IMAGE_SIZE, IMAGE_SIZE);
        ASSERT_NO_THROW(translucentImage.allocateOnDevice());
        compute::applyTranslucency(inputs.settings, config);
        program->renderScene(context->GetQueue(), *primitives, translucentImage, inputs, 0u,
                             IMAGE_SIZE);
        auto const translucentLuminance = luminanceOfCentrePixel(translucentImage);

        // sigma = -ln(1 - 0.1) / 1mm over the 5mm near box leaves ~0.59 transmittance, so the
        // far box contributes roughly half its own shading on top of the near box.
        EXPECT_GT(translucentLuminance, opaqueLuminance * 1.2f);
    }
} // namespace gladius::tests
