#include "compute/ComputeBackend.h"
#include "compute/ComputeBackendSettings.h"
#include "compute/IComputeBackend.h"
#include "webgpu/WebGPUDispatchPolicy.h"
#include "webgpu/WebGPUShaderAbi.h"

#include "ConfigManager.h"

#if defined(GLADIUS_ENABLE_WEBGPU)
#include "webgpu/WebGPUComputeBackend.h"
#include "webgpu/WebGPUComputeRenderer.h"
#endif

#include <chrono>
#include <cstdlib>

#include <gtest/gtest.h>

namespace gladius::compute::tests
{
    TEST(ComputeBackend, ToString_WithKnownBackend_ReturnsStableConfigurationValue)
    {
        EXPECT_EQ(toString(ComputeBackendKind::OpenCL), "opencl");
        EXPECT_EQ(toString(ComputeBackendKind::WebGPU), "webgpu");
    }

    TEST(ComputeBackend, ParseComputeBackend_WithKnownValue_ReturnsBackend)
    {
        EXPECT_EQ(parseComputeBackend("opencl"), ComputeBackendKind::OpenCL);
        EXPECT_EQ(parseComputeBackend("webgpu"), ComputeBackendKind::WebGPU);
    }

    TEST(ComputeBackend, ParseComputeBackend_WithUnknownValue_ReturnsEmpty)
    {
        EXPECT_FALSE(parseComputeBackend("OpenCL").has_value());
        EXPECT_FALSE(parseComputeBackend("cpu").has_value());
        EXPECT_FALSE(parseComputeBackend("").has_value());
    }

    TEST(ComputeBackend, IsComputeBackendBuilt_WithDisabledWebGpu_ReturnsFalse)
    {
#if defined(GLADIUS_ENABLE_WEBGPU)
        EXPECT_TRUE(isComputeBackendBuilt(ComputeBackendKind::WebGPU));
#else
        EXPECT_FALSE(isComputeBackendBuilt(ComputeBackendKind::WebGPU));
#endif
    }

    TEST(ComputeBackendSettings, UnavailableConfiguredBackend_FallsBackToBuiltBackend)
    {
        ConfigManager configManager;
        setConfiguredComputeBackend(configManager, ComputeBackendKind::WebGPU);

        auto const selectedBackend = getConfiguredComputeBackend(configManager);

        EXPECT_TRUE(isComputeBackendBuilt(selectedBackend));
        if (!isComputeBackendBuilt(ComputeBackendKind::WebGPU))
        {
            EXPECT_EQ(selectedBackend, ComputeBackendKind::OpenCL);
        }
    }

    TEST(ComputeBackend, SliceRequest_WithDefaultValues_HasValidUnitScale)
    {
        SliceRequest const request;

        EXPECT_EQ(request.width, 0u);
        EXPECT_EQ(request.height, 0u);
        EXPECT_FLOAT_EQ(request.sliceZ, 0.0f);
        EXPECT_FLOAT_EQ(request.scale, 1.0f);
        EXPECT_TRUE(request.shaderSource.empty());
    }

    TEST(ComputeBackend, SliceResult_WithPackedPixels_PreservesDimensionsAndPixels)
    {
        SliceResult const result{.width = 2u, .height = 1u, .pixels = {0x11223344u, 0x55667788u}};

        EXPECT_EQ(result.width, 2u);
        EXPECT_EQ(result.height, 1u);
        ASSERT_EQ(result.pixels.size(), 2u);
        EXPECT_EQ(result.pixels[0], 0x11223344u);
        EXPECT_EQ(result.pixels[1], 0x55667788u);
    }

    TEST(WebGPUShaderAbi, SliceUniforms_HasWebGpuCompatibleLayout)
    {
        EXPECT_EQ(sizeof(webgpu::SliceUniforms), 16u);
        EXPECT_EQ(alignof(webgpu::SliceUniforms), 16u);
        EXPECT_EQ(offsetof(webgpu::SliceUniforms, sliceZ), 0u);
        EXPECT_EQ(offsetof(webgpu::SliceUniforms, width), 4u);
        EXPECT_EQ(offsetof(webgpu::SliceUniforms, height), 8u);
        EXPECT_EQ(offsetof(webgpu::SliceUniforms, scale), 12u);
    }

    TEST(WebGPUShaderAbi, FrameUniforms_HasWebGpuCompatibleLayout)
    {
        EXPECT_EQ(sizeof(webgpu::FrameUniforms), 160u);
        EXPECT_EQ(alignof(webgpu::FrameUniforms), 16u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, eyeAndMaxDistance), 0u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, forwardAndHorizontalScale), 16u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, rightAndWidth), 32u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, upAndHeight), 48u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, verticalScaleAndMaxSteps), 64u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, firstRowAndCount), 80u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, timeSliceQualityNormal), 96u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, flagsModeReserved), 112u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, clippingBoxMin), 128u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, clippingBoxMax), 144u);
    }

    TEST(WebGPUDispatchPolicy, CalculateSliceDispatchSize_WithPartialWorkgroups_RoundsUp)
    {
        auto const dispatchSize = webgpu::calculateSliceDispatchSize(17u, 33u);

        ASSERT_TRUE(dispatchSize.has_value());
        EXPECT_EQ(dispatchSize->workgroupsX, 2u);
        EXPECT_EQ(dispatchSize->workgroupsY, 3u);
        EXPECT_EQ(dispatchSize->outputSizeBytes, 17u * 33u * sizeof(std::uint32_t));
    }

    TEST(WebGPUDispatchPolicy, CalculateSliceDispatchSize_WithZeroDimension_ReturnsEmpty)
    {
        EXPECT_FALSE(webgpu::calculateSliceDispatchSize(0u, 16u).has_value());
        EXPECT_FALSE(webgpu::calculateSliceDispatchSize(16u, 0u).has_value());
    }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(WebGPUComputeBackend, SubmitSlice_WithHeadlessDevice_ReturnsReadbackPixels)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUComputeBackend> backend;
        try
        {
            backend = std::make_unique<webgpu::WebGPUComputeBackend>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(backend->isAvailable());
        auto submission = backend->submitSlice(
          SliceRequest{.width = 17u, .height = 33u, .sliceZ = 0.0f, .scale = 1.0f});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 17u * 33u);
        EXPECT_EQ(result->pixels.front(), 0xFF1F1F1Fu);
        EXPECT_EQ(result->pixels[(16u * 17u) + 8u], 0xFFEBEBEBu);
    }

    TEST(WebGPUComputeBackend, SubmitFrame_WithHeadlessDevice_RayMarchesCameraFacingModel)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUComputeBackend> backend;
        try
        {
            backend = std::make_unique<webgpu::WebGPUComputeBackend>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        auto submission = backend->submitFrame(FrameRequest{.width = 33u,
                                                             .height = 33u,
                                                             .eyePosition = {0.0f, 0.0f, 2.0f},
                                                             .maxTravelDistance = 10.0f});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->pixels.size(), 33u * 33u);
        EXPECT_EQ(result->pixels.front(), 0xFF1A1A1Au);
        EXPECT_GT((result->pixels[(16u * 33u) + 16u] >> 16u) & 0xFFu, 100u);
    }

    TEST(WebGPUComputeBackend, SubmitFrame_WithDesktopViewportPixelCount_IsAccepted)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUComputeBackend> backend;
        try
        {
            backend = std::make_unique<webgpu::WebGPUComputeBackend>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(backend->isAvailable());
        auto submission = backend->submitFrame(
          FrameRequest{.width = 3685u,
                       .height = 1855u,
                       .endRow = 1u,
                       .maxTravelDistance = 10.0f});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->pixels.size(), 3685u);
    }

    TEST(WebGPUComputeRenderer, SubmitFrame_WithMaterializedAnalyticScene_ReturnsFrame)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUComputeRenderer> renderer;
        try
        {
            renderer = std::make_unique<webgpu::WebGPUComputeRenderer>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        auto scene = renderer->materializeScene(
          RenderSceneSnapshot{.sceneGeneration = 1u,
                              .analyticEvaluatorWgsl = "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { return vec4<f32>(vec3<f32>(0.8, 0.4, 0.2), length(position) - 0.5); }"});
        auto submission = renderer->submitFrame(
                    *scene, RenderRequest{.viewport = {.width = 33u, .height = 33u, .firstRow = 10u, .endRow = 20u}});
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (submission->getStatus() == RenderSubmissionStatus::Pending &&
               std::chrono::steady_clock::now() < deadline)
        {
            submission->progress();
        }

        ASSERT_EQ(submission->getStatus(), RenderSubmissionStatus::Succeeded) << submission->getErrorMessage();
        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        EXPECT_TRUE(frame->isValid());
        EXPECT_EQ(frame->firstRow, 10u);
        EXPECT_EQ(frame->endRow, 20u);
        EXPECT_EQ(frame->pixels.size(), 33u * 10u);
    }
#endif
}