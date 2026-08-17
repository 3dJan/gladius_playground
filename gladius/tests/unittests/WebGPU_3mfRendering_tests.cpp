#include "Document.h"
#include "EventLogger.h"
#include "compute/ComputeCore.h"
#include "compute/ComputeRendererFactory.h"
#include "nodes/Assembly.h"
#include "webgpu/WebGPUComputeBackend.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUModelSliceRequestFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gladius::tests
{
#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(WebGPU3mfRendering, SimpleGyroid3mf_RendersNonBackgroundPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!context->isValid())
        {
            GTEST_SKIP() << "OpenCL context unavailable for 3MF loading";
        }

        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);
        Document document(core);

        auto const filePath = std::filesystem::path("testdata") / "SimpleGyroid.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        auto const assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));

        auto request = webgpu::WebGPUModelSliceRequestFactory::createFrame(
          *assembly,
          compute::FrameRequest{.width = 64u,
                                .height = 64u,
                                .firstRow = 0u,
                                .endRow = 64u,
                                .eyePosition = {200.0f, 200.0f, 800.0f},
                                .forwardDirection = {0.0f, 0.0f, -1.0f},
                                .rightDirection = {1.0f, 0.0f, 0.0f},
                                .upDirection = {0.0f, 1.0f, 0.0f},
                                .horizontalScale = 0.5f,
                                .verticalScale = 0.5f,
                                .maxRaySteps = 512u,
                                .maxTravelDistance = 1000.0f});

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());
        auto submission = backend.submitFrame(std::move(request));
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();

        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 64u * 64u);

        auto const background = 0xFF1A1A1Au;
        auto const nonBackgroundPixels = std::count_if(result->pixels.begin(),
                                                       result->pixels.end(),
                                                       [background](std::uint32_t pixel)
                                                       { return pixel != background; });
        EXPECT_GT(nonBackgroundPixels, 64u);

        std::unordered_set<std::uint32_t> uniquePixels(result->pixels.begin(), result->pixels.end());
        ASSERT_GT(uniquePixels.size(), 4u)
          << "image has too little shading variation; first pixel: 0x" << std::hex << *uniquePixels.begin();

    }

    TEST(WebGPU3mfRendering, CorelessDocumentFallbackBounds_DoesNotFillVolumeWithBox)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto logger = std::make_shared<events::Logger>();
        Document document(logger);

        auto const filePath = std::filesystem::path("testdata") / "SimpleGyroid.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        auto const assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);

        auto const snapshot =
          compute::ComputeRendererFactory::materializeScene(assembly.get(),
                                                             *assembly->assemblyModel(),
                                                             /*generation=*/1u);
        ASSERT_TRUE(snapshot.isValid()) << "materializeScene failed without OpenCL core";

        // Reproduce the live neutral path: fallback [0,400]^3 bounds and default orbital camera.
        constexpr float pitch = 0.6f;
        constexpr float yaw = -1.6f;
        constexpr float dist = 800.0f;
        constexpr std::array<float, 3> lookAt = {200.0f, 200.0f, 50.0f};
        std::array<float, 3> const eye = {
          lookAt[0] + dist * std::cos(yaw) * std::cos(pitch),
          lookAt[1] + dist * std::sin(yaw) * std::cos(pitch),
          lookAt[2] + dist * std::sin(pitch)};

        std::array<float, 3> forward = {lookAt[0] - eye[0], lookAt[1] - eye[1], lookAt[2] - eye[2]};
        auto const normalize = [](std::array<float, 3> v)
        {
            float const len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            return std::array<float, 3>{v[0] / len, v[1] / len, v[2] / len};
        };
        forward = normalize(forward);
        std::array<float, 3> const up = {0.0f, 0.0f, 1.0f};
        std::array<float, 3> right = {
          forward[1] * up[2] - forward[2] * up[1],
          forward[2] * up[0] - forward[0] * up[2],
          forward[0] * up[1] - forward[1] * up[0]};
        right = normalize(right);
        std::array<float, 3> const cameraUp = {
          right[1] * forward[2] - right[2] * forward[1],
          right[2] * forward[0] - right[0] * forward[2],
          right[0] * forward[1] - right[1] * forward[0]};

        auto request = webgpu::WebGPUModelSliceRequestFactory::createFrame(
          *assembly,
          compute::FrameRequest{.width = 128u,
                                .height = 128u,
                                .firstRow = 0u,
                                .endRow = 128u,
                                .eyePosition = eye,
                                .forwardDirection = forward,
                                .rightDirection = right,
                                .upDirection = cameraUp,
                                .horizontalScale = 0.5f,
                                .verticalScale = 0.5f,
                                .maxRaySteps = 2000u,
                                .maxTravelDistance = 100000.0f,
                                .modelBounds = compute::RenderBounds{
                                  .min = {0.0f, 0.0f, 0.0f},
                                  .max = {400.0f, 400.0f, 400.0f}}});

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());
        auto submission = backend.submitFrame(std::move(request));
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();

        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 128u * 128u);

        auto const background = 0xFF1A1A1Au;
        auto const nonBackground = std::count_if(result->pixels.begin(),
                                                 result->pixels.end(),
                                                 [background](std::uint32_t pixel)
                                                 { return pixel != background; });
        std::unordered_set<std::uint32_t> uniquePixels(result->pixels.begin(), result->pixels.end());

        EXPECT_GT(nonBackground, 128u) << "frame is almost entirely background";
        EXPECT_GT(uniquePixels.size(), 4u) << "frame has no shading variation; likely a solid box";

        // If the volume were filled with a solid box, most pixels would share the same color.
        auto const dominantPixelCount = std::max_element(
          uniquePixels.begin(),
          uniquePixels.end(),
          [&result](std::uint32_t a, std::uint32_t b)
          {
              return std::count(result->pixels.begin(), result->pixels.end(), a)
                     < std::count(result->pixels.begin(), result->pixels.end(), b);
          });
        if (dominantPixelCount != uniquePixels.end())
        {
            auto const dominantCount = static_cast<std::uint32_t>(
              std::count(result->pixels.begin(), result->pixels.end(), *dominantPixelCount));
            EXPECT_LT(dominantCount, result->pixels.size() / 2u)
              << "more than half the frame is one color; volume appears filled";
        }
    }

    TEST(WebGPU3mfRendering, BuildPlateFlagAddsGeometryToAnalyticFrame)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        constexpr std::string_view evaluator = R"(
        fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
            let distance = 1000.0;
            return vec4<f32>(vec3<f32>(0.8, 0.4, 0.2), distance);
        }
        )";

        auto createRequest = [&evaluator](std::uint32_t const flags)
        {
            return compute::FrameRequest{.width = 64u,
                                          .height = 64u,
                                          .firstRow = 0u,
                                          .endRow = 64u,
                                          .eyePosition = {200.0f, 200.0f, 800.0f},
                                          .forwardDirection = {0.0f, 0.0f, -1.0f},
                                          .rightDirection = {1.0f, 0.0f, 0.0f},
                                          .upDirection = {0.0f, 1.0f, 0.0f},
                                          .horizontalScale = 0.5f,
                                          .verticalScale = 0.5f,
                                          .maxRaySteps = 512u,
                                          .maxTravelDistance = 1000.0f,
                                          .renderingFlags = flags,
                                          .shaderSource = webgpu::WebGPUFrameShaderComposer::compose(evaluator)};
        };

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());
        auto withoutPlatform = backend.submitFrame(createRequest(RF_DISABLE_ADAPTIVE_OMEGA));
        withoutPlatform->wait();
        ASSERT_EQ(withoutPlatform->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << withoutPlatform->getErrorMessage();
        auto withoutPlatformResult = withoutPlatform->takeResult();
        ASSERT_TRUE(withoutPlatformResult.has_value());

        auto withPlatform = backend.submitFrame(createRequest(RF_SHOW_BUILDPLATE | RF_DISABLE_ADAPTIVE_OMEGA));
        withPlatform->wait();
        ASSERT_EQ(withPlatform->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << withPlatform->getErrorMessage();
        auto withPlatformResult = withPlatform->takeResult();
        ASSERT_TRUE(withPlatformResult.has_value());

        std::size_t changedPixels = 0u;
        for (std::size_t index = 0u; index < withoutPlatformResult->pixels.size(); ++index)
        {
            changedPixels += withoutPlatformResult->pixels[index] != withPlatformResult->pixels[index] ? 1u : 0u;
        }
        EXPECT_GT(changedPixels, 64u);
    }

    TEST(WebGPU3mfRendering, CutoffIntersectsModelWithBoundingBox)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        constexpr std::string_view evaluator = R"(
        fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
            let distance = length(position - vec3<f32>(300.0, 50.0, 50.0)) - 20.0;
            return vec4<f32>(vec3<f32>(0.8, 0.4, 0.2), distance);
        }
        )";

        auto createRequest = [&evaluator](std::uint32_t const flags)
        {
            return compute::FrameRequest{.width = 64u,
                                          .height = 64u,
                                          .firstRow = 0u,
                                          .endRow = 64u,
                                          .eyePosition = {300.0f, 50.0f, 200.0f},
                                          .forwardDirection = {0.0f, 0.0f, -1.0f},
                                          .rightDirection = {1.0f, 0.0f, 0.0f},
                                          .upDirection = {0.0f, 1.0f, 0.0f},
                                          .horizontalScale = 0.5f,
                                          .verticalScale = 0.5f,
                                          .maxRaySteps = 512u,
                                          .maxTravelDistance = 500.0f,
                                          .sliceHeight = 100.0f,
                                          .renderingFlags = flags,
                                          .modelBounds = compute::RenderBounds{.min = {0.0f, 0.0f, 0.0f},
                                                                                .max = {100.0f, 100.0f, 100.0f}},
                                          .shaderSource = webgpu::WebGPUFrameShaderComposer::compose(evaluator)};
        };

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());

        auto unbounded = backend.submitFrame(createRequest(RF_DISABLE_ADAPTIVE_OMEGA));
        unbounded->wait();
        ASSERT_EQ(unbounded->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << unbounded->getErrorMessage();
        auto unboundedResult = unbounded->takeResult();
        ASSERT_TRUE(unboundedResult.has_value());

        auto clipped = backend.submitFrame(
          createRequest(RF_CUT_OFF_OBJECT | RF_DISABLE_ADAPTIVE_OMEGA));
        clipped->wait();
        ASSERT_EQ(clipped->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << clipped->getErrorMessage();
        auto clippedResult = clipped->takeResult();
        ASSERT_TRUE(clippedResult.has_value());

        auto const background = 0xFF1A1A1Au;
        auto const countNonBackground = [background](std::vector<std::uint32_t> const & pixels)
        {
            return std::count_if(pixels.begin(), pixels.end(), [background](std::uint32_t pixel)
                                 { return pixel != background; });
        };
        auto const unboundedPixels = countNonBackground(unboundedResult->pixels);
        auto const clippedPixels = countNonBackground(clippedResult->pixels);
        EXPECT_GT(unboundedPixels, 16u);
        EXPECT_LT(clippedPixels, unboundedPixels / 4u);
    }

    TEST(WebGPU3mfRendering, FieldOverlayUsesModelBoundingBox)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        constexpr std::string_view evaluator = R"(
        fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
            return vec4<f32>(vec3<f32>(0.8, 0.4, 0.2), 1.25);
        }
        )";

        auto createRequest = [&evaluator](compute::RenderBounds modelBounds)
        {
            return compute::FrameRequest{.width = 128u,
                                          .height = 128u,
                                          .firstRow = 0u,
                                          .endRow = 128u,
                                          .eyePosition = {200.0f, 200.0f, 200.0f},
                                          .forwardDirection = {0.0f, 0.0f, -1.0f},
                                          .rightDirection = {1.0f, 0.0f, 0.0f},
                                          .upDirection = {0.0f, 1.0f, 0.0f},
                                          .horizontalScale = 0.5f,
                                          .verticalScale = 0.5f,
                                          .maxRaySteps = 512u,
                                          .maxTravelDistance = 500.0f,
                                          .sliceHeight = 50.0f,
                                          .renderingFlags = RF_SHOW_FIELD | RF_DISABLE_ADAPTIVE_OMEGA |
                                                            RF_DISABLE_SHADOWS | RF_DISABLE_AO,
                                          .modelBounds = modelBounds,
                                          .shaderSource = webgpu::WebGPUFrameShaderComposer::compose(evaluator)};
        };

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());

        auto smallBounds = backend.submitFrame(
          createRequest(compute::RenderBounds{.min = {185.0f, 185.0f, 0.0f},
                                               .max = {215.0f, 215.0f, 100.0f}}));
        smallBounds->wait();
        ASSERT_EQ(smallBounds->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << smallBounds->getErrorMessage();
        auto smallResult = smallBounds->takeResult();
        ASSERT_TRUE(smallResult.has_value());

        auto largeBounds = backend.submitFrame(
          createRequest(compute::RenderBounds{.min = {0.0f, 0.0f, 0.0f},
                                               .max = {400.0f, 400.0f, 100.0f}}));
        largeBounds->wait();
        ASSERT_EQ(largeBounds->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << largeBounds->getErrorMessage();
        auto largeResult = largeBounds->takeResult();
        ASSERT_TRUE(largeResult.has_value());

        auto const background = 0xFF1A1A1Au;
        auto const countNonBackground = [background](std::vector<std::uint32_t> const & pixels)
        {
            return std::count_if(pixels.begin(), pixels.end(), [background](std::uint32_t pixel)
                                 { return pixel != background; });
        };
        auto const smallPixels = countNonBackground(smallResult->pixels);
        auto const largePixels = countNonBackground(largeResult->pixels);
        EXPECT_GT(smallPixels, 100u);
        EXPECT_GT(largePixels, smallPixels * 2u);
    }

    TEST(WebGPU3mfRendering, DefaultAppCameraAndFlags_RendersPlateAndPart)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
        if (!context->isValid())
        {
            GTEST_SKIP() << "OpenCL context unavailable for 3MF loading";
        }

        auto logger = std::make_shared<events::Logger>();
        auto core = std::make_shared<ComputeCore>(context, RequiredCapabilities::ComputeOnly, logger);
        Document document(core);

        auto const filePath = std::filesystem::path("testdata") / "SimpleGyroid.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        auto const assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);
        ASSERT_NE(assembly->assemblyModel(), nullptr);
        ASSERT_TRUE(core->updateParameterBlocking(*assembly));
        auto const boundingBox = core->getBoundingBox();
        ASSERT_TRUE(boundingBox.has_value());

        // Reproduce the live app's default view: orbital camera above the plate center,
        // pitch=0.6, yaw=-1.6, distance=800, all default overlay flags enabled.
        constexpr float pitch = 0.6f;
        constexpr float yaw = -1.6f;
        constexpr float dist = 800.0f;
        constexpr std::array<float, 3> lookAt = {200.0f, 200.0f, 50.0f};
        std::array<float, 3> const eye = {
          lookAt[0] + dist * std::cos(yaw) * std::cos(pitch),
          lookAt[1] + dist * std::sin(yaw) * std::cos(pitch),
          lookAt[2] + dist * std::sin(pitch)};

        std::array<float, 3> forward = {lookAt[0] - eye[0], lookAt[1] - eye[1], lookAt[2] - eye[2]};
        auto const normalize = [](std::array<float, 3> v)
        {
            float const len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            return std::array<float, 3>{v[0] / len, v[1] / len, v[2] / len};
        };
        forward = normalize(forward);
        std::array<float, 3> const up = {0.0f, 0.0f, 1.0f};
        std::array<float, 3> right = {
          forward[1] * up[2] - forward[2] * up[1],
          forward[2] * up[0] - forward[0] * up[2],
          forward[0] * up[1] - forward[1] * up[0]};
        right = normalize(right);
        std::array<float, 3> const cameraUp = {
          right[1] * forward[2] - right[2] * forward[1],
          right[2] * forward[0] - right[0] * forward[2],
          right[0] * forward[1] - right[1] * forward[0]};

        constexpr std::uint32_t kDefaultFlags = 1u | 2u | 4u | 16u; // plate + cutoff + field + axes
        auto request = webgpu::WebGPUModelSliceRequestFactory::createFrame(
          *assembly,
          compute::FrameRequest{.width = 128u,
                                .height = 128u,
                                .firstRow = 0u,
                                .endRow = 128u,
                                .eyePosition = eye,
                                .forwardDirection = forward,
                                .rightDirection = right,
                                .upDirection = cameraUp,
                                .horizontalScale = 0.5f,
                                .verticalScale = 0.5f,
                                .maxRaySteps = 512u,
                                .maxTravelDistance = 100000.0f,
                                .sliceHeight = 1000.0f,
                                .renderingFlags = kDefaultFlags,
                                .modelBounds = compute::RenderBounds{
                                  .min = {boundingBox->min.x, boundingBox->min.y, boundingBox->min.z},
                                  .max = {boundingBox->max.x, boundingBox->max.y, boundingBox->max.z}}});

        webgpu::WebGPUComputeBackend backend;
        ASSERT_TRUE(backend.isAvailable());
        auto submission = backend.submitFrame(std::move(request));
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();

        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 128u * 128u);

        auto const background = 0xFF1A1A1Au;
        auto const nonBackground = std::count_if(result->pixels.begin(), result->pixels.end(),
                                                 [background](std::uint32_t pixel) { return pixel != background; });
        std::unordered_set<std::uint32_t> uniquePixels(result->pixels.begin(), result->pixels.end());

        EXPECT_GT(nonBackground, 128u) << "frame is almost entirely background";
        EXPECT_GT(uniquePixels.size(), 4u) << "frame has no shading variation";
    }
#endif
}
