#include "compute/ComputeBackend.h"
#include "compute/ComputeBackendSettings.h"
#include "compute/ApplicationComputeRuntime.h"
#include "compute/ComputeRendererFactory.h"
#include "compute/IComputeBackend.h"
#include "compute/RenderBackendSession.h"
#include "Document.h"
#include "kernel/types.h"
#include "webgpu/WebGPUDispatchPolicy.h"
#include "webgpu/WebGPUFrameShaderComposer.h"
#include "webgpu/WebGPUShaderAbi.h"

#include "ConfigManager.h"

#if defined(GLADIUS_ENABLE_WEBGPU)
#include "webgpu/WebGPUComputeBackend.h"
#include "webgpu/WebGPUComputeRenderer.h"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

namespace gladius::compute::tests
{
    namespace
    {
        /// @brief Read the WebGPU frame shader source from the kernel source tree.
        std::string loadFrameShaderSource()
        {
            std::filesystem::path const path =
              std::filesystem::path{GLADIUS_KERNEL_SOURCE_DIR}.parent_path() / "webgpu" /
              "shaders" / "frame_compute.wgsl";
            std::ifstream file(path);
            if (!file)
            {
                return {};
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
    }

    TEST(ComputeBackend, ToString_WithKnownBackend_ReturnsStableConfigurationValue)
    {
        EXPECT_EQ(toString(ComputeBackendKind::OpenCL), "opencl");
        EXPECT_EQ(toString(ComputeBackendKind::WebGPU), "webgpu");
    }

    TEST(ComputeBackendSettings, ExplicitConfiguredBackend_IsReturnedWithoutFallback)
    {
        ConfigManager configManager;
        setConfiguredComputeBackend(configManager, ComputeBackendKind::WebGPU);

        auto const preference = getConfiguredComputeBackendPreference(configManager);

        ASSERT_TRUE(preference.has_value());
        EXPECT_EQ(*preference, ComputeBackendKind::WebGPU);
    }

    TEST(ComputeBackendSettings, InvalidConfiguredBackend_HasNoExplicitPreference)
    {
        ConfigManager configManager;
        configManager.setValue("compute", "backend", std::string{"invalid"});

        EXPECT_FALSE(getConfiguredComputeBackendPreference(configManager).has_value());
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

    TEST(ComputeBackend, GetComputeBackendAttemptOrder_WithEitherPreference_TriesBuiltBackendsOnce)
    {
        auto const openCLBuilt = isComputeBackendBuilt(ComputeBackendKind::OpenCL);
        auto const webGPUBuilt = isComputeBackendBuilt(ComputeBackendKind::WebGPU);

        for (auto const preferredBackend : {ComputeBackendKind::OpenCL, ComputeBackendKind::WebGPU})
        {
            auto const attemptOrder = getComputeBackendAttemptOrder(preferredBackend);

            EXPECT_EQ(attemptOrder.size(), static_cast<std::size_t>(openCLBuilt + webGPUBuilt));
            if (attemptOrder.empty())
            {
                EXPECT_FALSE(openCLBuilt);
                EXPECT_FALSE(webGPUBuilt);
                continue;
            }

            auto const expectedFirst = isComputeBackendBuilt(preferredBackend)
                                         ? preferredBackend
                                         : (preferredBackend == ComputeBackendKind::OpenCL
                                              ? ComputeBackendKind::WebGPU
                                              : ComputeBackendKind::OpenCL);
            EXPECT_EQ(attemptOrder.front(), expectedFirst);
            EXPECT_TRUE(isComputeBackendBuilt(attemptOrder.front()));
            if (attemptOrder.size() == 2u)
            {
                EXPECT_NE(attemptOrder[0], attemptOrder[1]);
                EXPECT_TRUE(isComputeBackendBuilt(attemptOrder[1]));
            }
        }
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

    TEST(ComputeBackendSettings, UnavailableOpenCLPreference_FallsBackToWebGPUInWebGPUOnlyBuild)
    {
#if defined(GLADIUS_ENABLE_WEBGPU) && !defined(GLADIUS_ENABLE_OPENCL)
        ConfigManager configManager;
        setConfiguredComputeBackend(configManager, ComputeBackendKind::OpenCL);

        EXPECT_EQ(getConfiguredComputeBackend(configManager), ComputeBackendKind::WebGPU);
#else
        GTEST_SKIP() << "Requires a WebGPU-only build";
#endif
    }

    TEST(Document, CorelessConstruction_CreatesAnalyticDocumentState)
    {
        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document(logger);

        EXPECT_EQ(document.getCore(), nullptr);
        ASSERT_TRUE(document.getAssembly());
        ASSERT_TRUE(document.getAssembly()->assemblyModel());
    }

    TEST(Document, RefreshModelBlocking_WithoutComputeCore_UpdatesFlatAssembly)
    {
        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document(logger);
        auto const previousFlatAssembly = document.getFlatAssembly();

        ASSERT_NO_THROW(document.refreshModelBlocking());

        ASSERT_TRUE(document.getFlatAssembly());
        EXPECT_NE(document.getFlatAssembly(), previousFlatAssembly);
        EXPECT_EQ(document.getCore(), nullptr);
    }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(ApplicationComputeRuntime, WebGPUConstruction_DoesNotCreateOpenCLCore)
    {
        std::unique_ptr<compute::IBackendRuntime> runtime;
        try
        {
            runtime = compute::ApplicationComputeRuntime::createWebGPU();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(runtime);
        EXPECT_EQ(runtime->getBackendKind(), ComputeBackendKind::WebGPU);
        EXPECT_EQ(runtime->getOpenCLCore(), nullptr);
        ASSERT_TRUE(runtime->isAvailable()) << runtime->getErrorMessage();
        EXPECT_NE(runtime->getRenderBackendSession(), nullptr);
    }

    TEST(ApplicationComputeRuntime, WebGPUCorelessDocument_CanMaterializeAndRender)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document(logger);

        std::unique_ptr<compute::IBackendRuntime> runtime;
        try
        {
            runtime = compute::ApplicationComputeRuntime::createWebGPU();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(runtime->isAvailable()) << runtime->getErrorMessage();
        auto * const session = runtime->getRenderBackendSession();
        ASSERT_NE(session, nullptr);

        auto const snapshot = compute::ComputeRendererFactory::materializeScene(
          document.getAssembly().get(), *document.getAssembly()->assemblyModel(), 1u);
        ASSERT_TRUE(session->replaceScene(snapshot));

        auto submission = session->submitFrame(
          compute::RenderRequest{.viewport = {.width = 17u, .height = 17u, .firstRow = 0u, .endRow = 17u}});
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (submission->getStatus() == compute::RenderSubmissionStatus::Pending &&
               std::chrono::steady_clock::now() < deadline)
        {
            submission->progress();
        }

        ASSERT_EQ(submission->getStatus(), compute::RenderSubmissionStatus::Succeeded)
          << submission->getErrorMessage();
        auto frame = submission->takeFrame();
        ASSERT_TRUE(frame.has_value());
        EXPECT_TRUE(frame->isValid());
    }
#endif

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
        EXPECT_EQ(sizeof(webgpu::FrameUniforms), 176u);
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
        EXPECT_EQ(offsetof(webgpu::FrameUniforms, translucencyParams), 160u);
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

    TEST(WebGPUDispatchPolicy, CalculateSliceDispatchSize_WithWideWorkgroup_RoundsUpPerShape)
    {
        auto const dispatchSize = webgpu::calculateSliceDispatchSize(
          17u, 33u, compute::WorkgroupShape{.width = 32u, .height = 8u});

        ASSERT_TRUE(dispatchSize.has_value());
        EXPECT_EQ(dispatchSize->workgroupsX, 1u);
        EXPECT_EQ(dispatchSize->workgroupsY, 5u);
        EXPECT_EQ(dispatchSize->outputSizeBytes, 17u * 33u * sizeof(std::uint32_t));
    }

    TEST(WebGPUDispatchPolicy, CalculateSliceDispatchSize_WithInvalidWorkgroup_ReturnsEmpty)
    {
        EXPECT_FALSE(webgpu::calculateSliceDispatchSize(
                       16u, 16u, compute::WorkgroupShape{.width = 0u, .height = 16u})
                       .has_value());
        EXPECT_FALSE(webgpu::calculateSliceDispatchSize(
                       16u, 16u, compute::WorkgroupShape{.width = 16u, .height = 17u})
                       .has_value());
    }

    TEST(WebGPUWorkgroupShape, IsValid_TileWithinInvocationLimit)
    {
        EXPECT_TRUE(compute::WorkgroupShape{}.isValid());
        EXPECT_TRUE(compute::WorkgroupShape{}.isDefault());
        EXPECT_TRUE((compute::WorkgroupShape{.width = 8u, .height = 32u}).isValid());
        EXPECT_FALSE((compute::WorkgroupShape{.width = 16u, .height = 17u}).isValid());
        EXPECT_FALSE((compute::WorkgroupShape{.width = 257u, .height = 1u}).isValid());
    }

    TEST(WebGPUFrameShaderComposer, ApplyWorkgroupShape_DefaultKeepsShaderUnchanged)
    {
        auto const composed = webgpu::WebGPUFrameShaderComposer::compose(
          "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { return vec4<f32>(vec3<f32>(1.0), 0.0); }");
        EXPECT_EQ(webgpu::WebGPUFrameShaderComposer::applyWorkgroupShape(composed, {}), composed);
    }

    TEST(WebGPUFrameShaderComposer, ApplyWorkgroupShape_NonDefault_RewritesDeclaration)
    {
        auto const composed = webgpu::WebGPUFrameShaderComposer::compose(
          "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { return vec4<f32>(vec3<f32>(1.0), 0.0); }");
        auto const rewritten = webgpu::WebGPUFrameShaderComposer::applyWorkgroupShape(
          composed, compute::WorkgroupShape{.width = 8u, .height = 32u});

        EXPECT_NE(rewritten, composed);
        EXPECT_NE(rewritten.find("@workgroup_size(8, 32, 1)"), std::string::npos);
        EXPECT_EQ(rewritten.find("@workgroup_size(16, 16, 1)"), std::string::npos);
    }

    TEST(WebGPUFrameShaderComposer, ApplyWorkgroupShape_InvalidShape_ThrowsException)
    {
        EXPECT_THROW(
          [&]()
          {
              auto const rewritten = webgpu::WebGPUFrameShaderComposer::applyWorkgroupShape(
                "@compute @workgroup_size(16, 16, 1)",
                compute::WorkgroupShape{.width = 16u, .height = 17u});
              static_cast<void>(rewritten);
          }(),
          std::invalid_argument);
    }

    TEST(WebGPUShaderAbi, SkipBackgroundRaysFlag_MirrorsKernelAndShader)
    {
        EXPECT_EQ(webgpu::RF_SKIP_BACKGROUND_RAYS, RF_SKIP_BACKGROUND_RAYS);
        EXPECT_EQ(webgpu::RF_SKIP_BACKGROUND_RAYS, 32u);

        auto const source = loadFrameShaderSource();
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("const RF_SKIP_BACKGROUND_RAYS: u32 = 32u;"), std::string::npos);
        EXPECT_NE(source.find("fn background_skip_bounds()"), std::string::npos);
        EXPECT_NE(source.find("fn ray_misses_bounds("), std::string::npos);
        EXPECT_NE(source.find("ray_misses_bounds(eye, ray_direction, max_distance)"),
                  std::string::npos);
        EXPECT_NE(source.find("frame.vertical_scale_and_max_steps.z"), std::string::npos);
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

    TEST(WebGPUComputeBackend, SubmitFrame_WithTimingAndMetricsRequested_ReturnsBoth)
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
                                                             .maxTravelDistance = 10.0f,
                                                             .collectGpuTiming = true,
                                                             .collectMetrics = true});
        submission->wait();

        ASSERT_EQ(submission->getStatus(), ComputeCompletionStatus::Succeeded)
          << submission->getErrorMessage();
        auto const result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->pixels.size(), 33u * 33u);

        auto const timing = submission->getFrameTiming();
        ASSERT_TRUE(timing.has_value());
        EXPECT_GE(timing->submitToWorkDoneMs, 0.0);

        auto const metrics = submission->getFrameMetrics();
        ASSERT_TRUE(metrics.has_value());
        EXPECT_EQ(metrics->totalRays, 33u * 33u);
        EXPECT_GT(metrics->totalSteps, 0u);
        EXPECT_GT(metrics->totalHits, 0u);
        EXPECT_LE(metrics->totalHits, metrics->totalRays);
    }

    TEST(WebGPUComputeBackend, SubmitFrame_WithBackgroundSkipFlag_MissesBoundsWithoutMarching)
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

        auto const run = [&](std::uint32_t const flags)
          -> std::pair<std::optional<FrameResult>, std::optional<FrameMetrics>>
        {
            auto submission = backend->submitFrame(
              FrameRequest{.width = 33u,
                           .height = 33u,
                           .eyePosition = {2000.0f, 2000.0f, 2000.0f},
                           .maxTravelDistance = 10.0f,
                           .renderingFlags = flags,
                           .modelBounds = RenderBounds{.min = {1000.0f, 1000.0f, 1000.0f},
                                                       .max = {1100.0f, 1100.0f, 1100.0f}},
                           .boundsErrorBound = 8.0f,
                           .collectMetrics = true});
            submission->wait();
            EXPECT_EQ(submission->getStatus(), ComputeCompletionStatus::Succeeded)
              << submission->getErrorMessage();
            auto result = submission->takeResult();
            EXPECT_TRUE(result.has_value());
            if (result.has_value())
            {
                EXPECT_EQ(result->pixels.size(), 33u * 33u);
            }
            auto metrics = submission->getFrameMetrics();
            EXPECT_TRUE(metrics.has_value());
            return std::pair{result, metrics};
        };

        auto const [unskipped, unskippedMetrics] = run(0u);
        ASSERT_TRUE(unskipped.has_value());
        ASSERT_TRUE(unskippedMetrics.has_value());
        for (auto const pixel : unskipped->pixels)
        {
            EXPECT_EQ(pixel, 0xFF1A1A1Au);
        }

        auto const [skipped, skippedMetrics] = run(RF_SKIP_BACKGROUND_RAYS);
        ASSERT_TRUE(skipped.has_value());
        ASSERT_TRUE(skippedMetrics.has_value());
        for (auto const pixel : skipped->pixels)
        {
            EXPECT_EQ(pixel, 0xFF1A1A1Au);
        }

        EXPECT_EQ(skippedMetrics->totalRays, unskippedMetrics->totalRays);
        EXPECT_GT(unskippedMetrics->totalSteps, 0u);
        EXPECT_EQ(skippedMetrics->totalSteps, 0u);
        EXPECT_EQ(skippedMetrics->totalHits, 0u);
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