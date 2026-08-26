#include "Document.h"
#include "ImagePayloadSerializer.h"
#include "ResourceManager.h"
#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/ToWgslVisitor.h"
#include "webgpu/WebGPUComputeRenderer.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gladius::webgpu::tests
{
    namespace
    {
        constexpr ResourceId IMAGE_RESOURCE_ID = 2u;

        io::ImageStack createTestImageStack()
        {
            io::ImageStack stack(IMAGE_RESOURCE_ID);
            for (unsigned char layer = 0u; layer < 2u; ++layer)
            {
                io::ImageData data;
                data.reserve(2u * 2u * 4u);
                for (unsigned char pixel = 0u; pixel < 4u; ++pixel)
                {
                    auto const value = static_cast<unsigned char>(layer * 80u + pixel * 20u);
                    data.push_back(value);
                    data.push_back(static_cast<unsigned char>(value + 1u));
                    data.push_back(static_cast<unsigned char>(value + 2u));
                    data.push_back(static_cast<unsigned char>(value + 3u));
                }
                io::Image image(data, 2u, 2u);
                image.setFormat(io::PixelFormat::RGBA_8BIT);
                stack.push_back(image);
            }
            return stack;
        }

        std::unique_ptr<nodes::Model> createImageSamplerModel()
        {
            auto model = std::make_unique<nodes::Model>();
            model->createBeginEndWithDefaultInAndOuts();

            auto * resource = model->create<nodes::Resource>();
            auto * uvw = model->create<nodes::ConstantVector>();
            auto * sampler = model->create<nodes::ImageSampler>();
            resource->parameter().at(nodes::FieldNames::ResourceId).setValue(IMAGE_RESOURCE_ID);
            for (auto & [name, parameter] : uvw->parameter())
            {
                parameter.setValue(0.25f);
                parameter.setModifiable(false);
            }
            sampler->parameter().at(nodes::FieldNames::Filter).setValue(
              static_cast<int>(SamplingFilter::SF_LINEAR));
            sampler->parameter().at(nodes::FieldNames::TileStyleU).setValue(
              static_cast<int>(TextureTileStyle::TTS_CLAMP));
            sampler->parameter().at(nodes::FieldNames::TileStyleV).setValue(
              static_cast<int>(TextureTileStyle::TTS_CLAMP));
            sampler->parameter().at(nodes::FieldNames::TileStyleW).setValue(
              static_cast<int>(TextureTileStyle::TTS_CLAMP));

            EXPECT_TRUE(model->addLink(resource->getOutputValue().getId(),
                                       sampler->parameter().at(nodes::FieldNames::ResourceId).getId()));
            EXPECT_TRUE(model->addLink(uvw->getVectorOutputPort().getId(),
                                       sampler->parameter().at(nodes::FieldNames::UVW).getId()));
            EXPECT_TRUE(model->addLink(sampler->getOutputs().at(nodes::FieldNames::Color).getId(),
                                       model->getEndNode()->parameter().at(nodes::FieldNames::Color).getId()));
            EXPECT_TRUE(model->addLink(sampler->getOutputs().at(nodes::FieldNames::Alpha).getId(),
                                       model->getEndNode()->parameter().at(nodes::FieldNames::Shape).getId()));
            return model;
        }

        std::vector<float> evaluateImageExpression(std::vector<std::array<float, 3>> positions,
                                                   std::vector<float> const & payload,
                                                   std::string const & expression)
        {
            WebGPUSdfEvaluator evaluator;
            if (!evaluator.isAvailable())
            {
                return {};
            }

            auto const modelEvaluator =
              "fn evaluateModel(position: vec3<f32>) -> vec4<f32> {\n"
                            "    return vec4<f32>(0.0f, 0.0f, 0.0f, " + expression + ");\n"
              "}\n";
            std::vector<std::vector<float>> payloadTable(IMAGE_RESOURCE_ID + 1u);
            payloadTable[IMAGE_RESOURCE_ID] = payload;
                        compute::SdfEvaluationRequest request;
                        request.positions = std::move(positions);
                        request.shaderSource = WebGPUSdfShaderComposer::composeWithImageSupport(modelEvaluator);
                        request.imagePayloadTable = std::move(payloadTable);
                        EXPECT_EQ(request.imagePayloadTable.size(), IMAGE_RESOURCE_ID + 1u);
                        EXPECT_EQ(request.imagePayloadTable[IMAGE_RESOURCE_ID], payload);
                        auto result = evaluator.evaluate(std::move(request));
                        EXPECT_TRUE(evaluator.getErrorMessage().empty()) << evaluator.getErrorMessage();
                        return result.values;
        }

                std::vector<float> evaluateImageChannel(std::vector<std::array<float, 3>> positions,
                                                                                                std::vector<float> const & payload,
                                                                                                std::string_view const channel,
                                                                                                std::uint32_t const filter,
                                                                                                std::uint32_t const tileStyle = 2u)
                {
                        auto const expression =
                            "gladiusSampleImage(position, 2u, vec3<u32>(" + std::to_string(tileStyle) + "u), " +
                            std::to_string(filter) + "u)." + std::string(channel);
                        return evaluateImageExpression(std::move(positions), payload, expression);
                }
    }

    TEST(ImagePayloadSerializer, SerializeRgbaStack_StoresDimensionsAndFlipsRows)
    {
        auto const payload = io::serializeImageStackPayload(createTestImageStack());

        ASSERT_EQ(payload.size(), io::IMAGE_PAYLOAD_HEADER_FLOATS + 2u * 2u * 2u * 4u);
        EXPECT_FLOAT_EQ(payload[0], 2.0f);
        EXPECT_FLOAT_EQ(payload[1], 2.0f);
        EXPECT_FLOAT_EQ(payload[2], 2.0f);
        EXPECT_FLOAT_EQ(payload[4], 40.0f / 255.0f);
        EXPECT_FLOAT_EQ(payload[8], 60.0f / 255.0f);
        EXPECT_FLOAT_EQ(payload[12], 0.0f);
        EXPECT_FLOAT_EQ(payload[20], 120.0f / 255.0f);
    }

    TEST(ImagePayloadSerializer, SerializeOneBitGrayscale_MatchesLegacyChannelConversion)
    {
        io::ImageData data{128u};
        io::Image image(data, 1u, 1u);
        image.setFormat(io::PixelFormat::GRAYSCALE_1BIT);
        io::ImageStack stack(IMAGE_RESOURCE_ID);
        stack.push_back(image);

        auto const payload = io::serializeImageStackPayload(stack);

        ASSERT_EQ(payload.size(), io::IMAGE_PAYLOAD_HEADER_FLOATS + 4u);
        EXPECT_FLOAT_EQ(payload[4], 128.0f / 255.0f);
        EXPECT_FLOAT_EQ(payload[5], 1.0f);
        EXPECT_FLOAT_EQ(payload[6], 1.0f);
        EXPECT_FLOAT_EQ(payload[7], 1.0f);
    }

    TEST(ToWgslVisitor, VisitImageSampler_EmitsImageHookAndOutputs)
    {
        auto model = createImageSamplerModel();
        nodes::ToWgslVisitor visitor;
        ASSERT_NO_THROW(model->visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const wgsl = source.str();
        EXPECT_NE(wgsl.find("gladiusSampleImage("), std::string::npos);
        EXPECT_NE(wgsl.find("2u, vec3<u32>(2u, 2u, 2u), 1u"), std::string::npos);
        EXPECT_NE(wgsl.find("_rgba.xyz"), std::string::npos);
        EXPECT_NE(wgsl.find("_rgba.w"), std::string::npos);
    }

    TEST(AnalyticRenderSceneSnapshotFactory, ImageOnlyModel_MaterializesImagePayload)
    {
        ResourceManager resourceManager(nullptr, {});
        auto stack = createTestImageStack();
        resourceManager.addResource(
          ResourceKey{IMAGE_RESOURCE_ID, ResourceType::ImageStack}, std::move(stack));
        auto model = createImageSamplerModel();

        auto const snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *model, 1u, resourceManager);

        ASSERT_TRUE(snapshot.isValid());
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities,
                                           compute::RendererCapability::ImageSampling));
        EXPECT_FALSE(compute::hasCapability(snapshot.requiredCapabilities,
                                            compute::RendererCapability::MeshSdf));
        ASSERT_GT(snapshot.imageResources.size(), IMAGE_RESOURCE_ID);
        EXPECT_TRUE(snapshot.imageResources[IMAGE_RESOURCE_ID].isValid());
    }

    TEST(WebGPUFrameShaderComposer, ComposeWithImageSupport_IncludesModuleAndBindings)
    {
        auto const shader = WebGPUFrameShaderComposer::composeWithImageSupport(
          "fn evaluateModel(position: vec3<f32>) -> vec4<f32> {\n"
          "    return gladiusSampleImage(position, 0u, vec3<u32>(2u), 0u);\n"
          "}\n");

        EXPECT_NE(shader.find("fn gladiusSampleImage("), std::string::npos);
        EXPECT_NE(shader.find("@group(0) @binding(8)"), std::string::npos);
        EXPECT_NE(shader.find("@group(0) @binding(9)"), std::string::npos);
        EXPECT_EQ(shader.find("GLADIUS_IMAGE_SAMPLING_MODULE"), std::string::npos);
    }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(ImageStackWebGPU, NearestAndLinearSampling_MatchKnownValues)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto const payload = io::serializeImageStackPayload(createTestImageStack());
        auto const nearest = evaluateImageChannel({{0.0f, 0.0f, 0.0f},
                                                    {0.75f, 0.75f, 0.75f}},
                                                   payload,
                                                   "x",
                                                   0u);
        if (nearest.empty())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        ASSERT_EQ(nearest.size(), 2u);
        EXPECT_NEAR(nearest[0], 40.0f / 255.0f, 1.0e-6f);
        EXPECT_NEAR(nearest[1], 100.0f / 255.0f, 1.0e-6f);

        auto const linear = evaluateImageChannel({{0.25f, 0.25f, 0.25f}}, payload, "x", 1u);
        ASSERT_EQ(linear.size(), 1u);
        EXPECT_NEAR(linear[0], 70.0f / 255.0f, 1.0e-6f);
    }

    TEST(ImageStackWebGPU, TileStyles_RepeatMirrorAndClampMatchOpenClBehavior)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto const payload = io::serializeImageStackPayload(createTestImageStack());
        auto const repeat = evaluateImageChannel({{1.75f, 0.0f, 0.0f}}, payload, "x", 0u, 0u);
        auto const mirror = evaluateImageChannel({{1.75f, 0.0f, 0.0f}}, payload, "x", 0u, 1u);
        auto const clamp = evaluateImageChannel({{1.75f, 0.0f, 0.0f}}, payload, "x", 0u, 2u);
        if (repeat.empty() || mirror.empty() || clamp.empty())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        EXPECT_NEAR(repeat[0], 60.0f / 255.0f, 1.0e-6f);
        EXPECT_NEAR(mirror[0], 40.0f / 255.0f, 1.0e-6f);
        EXPECT_NEAR(clamp[0], 0.0f, 1.0e-6f);
    }

    TEST(ImageStackWebGPU, ImageStack3mf_RendersNonBackgroundPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document(logger);
        auto const filePath = std::filesystem::path("testdata") /
                              "SphereInACage_small.imagestack.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        auto const assembly = document.getFlatAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        auto snapshot = compute::AnalyticRenderSceneSnapshotFactory::create(
          *assembly, 3u, &document.getResourceManager());
        ASSERT_TRUE(snapshot.isValid());
        EXPECT_TRUE(compute::hasCapability(snapshot.requiredCapabilities,
                                           compute::RendererCapability::ImageSampling));
        EXPECT_FALSE(snapshot.imageResources.empty());
        EXPECT_NE(snapshot.analyticEvaluatorWgsl.find("gladiusSampleImage("), std::string::npos);

                auto const payloadIterator = std::find_if(snapshot.imageResources.begin(),
                                                                                                    snapshot.imageResources.end(),
                                                                                                    [](compute::MeshResourcePayload const & payload)
                                                                                                    { return payload.isValid(); });
                ASSERT_NE(payloadIterator, snapshot.imageResources.end());
                auto const [payloadMin, payloadMax] = std::minmax_element(
                    payloadIterator->data.begin() + io::IMAGE_PAYLOAD_HEADER_FLOATS,
                    payloadIterator->data.end());
                EXPECT_LT(*payloadMin, 0.5f);
                EXPECT_GT(*payloadMax, 0.5f);

                std::vector<std::array<float, 3>> probePositions;
                for (float z = 45.0f; z <= 55.0f; z += 1.0f)
                {
                    for (float y = 91.0f; y <= 101.0f; y += 1.0f)
                        {
                        for (float x = 80.0f; x <= 90.0f; x += 1.0f)
                                {
                                        probePositions.push_back({x, y, z});
                                }
                        }
                }
                std::vector<std::vector<float>> imagePayloadTable;
                imagePayloadTable.reserve(snapshot.imageResources.size());
                for (auto const & image : snapshot.imageResources)
                {
                        imagePayloadTable.push_back(image.data);
                }
                WebGPUSdfEvaluator evaluator;
                ASSERT_TRUE(evaluator.isAvailable());
                auto const probeValues = evaluator
                                                                     .evaluate(compute::SdfEvaluationRequest{
                                                                         .positions = std::move(probePositions),
                                                                         .shaderSource = WebGPUSdfShaderComposer::composeWithImageSupport(
                                                                             snapshot.analyticEvaluatorWgsl),
                                                                         .parameterValues = snapshot.parameterValues,
                                                                         .imagePayloadTable = std::move(imagePayloadTable)})
                                                                     .values;
                EXPECT_TRUE(evaluator.getErrorMessage().empty()) << evaluator.getErrorMessage();
                ASSERT_FALSE(probeValues.empty());
                auto const [minValue, maxValue] = std::minmax_element(probeValues.begin(), probeValues.end());
                EXPECT_LT(*minValue, 0.0f);
                EXPECT_GT(*maxValue, 0.0f);

        WebGPUComputeRenderer renderer;
        if (!renderer.isAvailable())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }
        auto scene = renderer.materializeScene(std::move(snapshot));
        auto submission = renderer.submitFrame(
          *scene,
          compute::RenderRequest{
            .camera = {.eyePosition = {84.72f, 96.07f, 80.0f},
                       .forwardDirection = {0.0f, 0.0f, -1.0f},
                       .rightDirection = {1.0f, 0.0f, 0.0f},
                       .upDirection = {0.0f, 1.0f, 0.0f}},
            .frustum = {.horizontalScale = 0.5f, .verticalScale = 0.5f},
            .settings = {.maxRaySteps = 1024u, .maxTravelDistance = 100.0f},
            .modelBounds = compute::RenderBounds{.min = {79.72f, 91.07f, 45.0f},
                                                  .max = {89.73f, 101.08f, 55.01f}},
            .viewport = {.width = 96u, .height = 96u, .firstRow = 0u, .endRow = 96u}});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), compute::RenderSubmissionStatus::Succeeded)
          << submission->getErrorMessage();
        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        ASSERT_TRUE(frame->isValid());
        constexpr std::uint32_t BACKGROUND = 0xFF1A1A1Au;
        auto const nonBackground = std::count_if(frame->pixels.begin(),
                                                 frame->pixels.end(),
                                                 [](std::uint32_t const pixel)
                                                 { return pixel != BACKGROUND; });
        EXPECT_GT(nonBackground, 96u);
    }
#endif
}
