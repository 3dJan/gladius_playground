#include "compute/ComputeBackend.h"
#include "compute/IComputeBackend.h"
#include "webgpu/WebGPUDispatchPolicy.h"
#include "webgpu/WebGPUShaderAbi.h"

#if defined(GLADIUS_ENABLE_WEBGPU)
#include "webgpu/WebGPUComputeBackend.h"
#endif

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
        EXPECT_EQ(sizeof(webgpu::FrameUniforms), 80u);
        EXPECT_EQ(alignof(webgpu::FrameUniforms), 16u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, eyeAndMaxDistance), 0u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, forwardAndHorizontalScale), 16u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, rightAndWidth), 32u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, upAndHeight), 48u);
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, verticalScaleAndMaxSteps), 64u);
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
        EXPECT_EQ(result->pixels.front(), 0xFF140D08u);
        EXPECT_GT((result->pixels[(16u * 33u) + 16u] >> 16u) & 0xFFu, 100u);
    }
#endif
}