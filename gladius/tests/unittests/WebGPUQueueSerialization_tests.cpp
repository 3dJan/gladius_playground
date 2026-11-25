#if defined(GLADIUS_ENABLE_WEBGPU)

#include "compute/ApplicationComputeRuntime.h"
#include "compute/BoundingBoxContracts.h"
#include "compute/RenderBackendSession.h"
#include "webgpu/WebGPUBoundsService.h"
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUComputeRenderer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] bool areWebGpuTestsEnabled()
        {
            return std::getenv("GLADIUS_RUN_WEBGPU_TESTS") != nullptr;
        }

        [[nodiscard]] compute::RenderSceneSnapshot makeSphereSnapshot(
          std::uint64_t const sceneGeneration)
        {
            return compute::RenderSceneSnapshot{
                .sceneGeneration = sceneGeneration,
                .requiredCapabilities = compute::RendererCapability::AnalyticRendering,
                .analyticEvaluatorWgsl =
                  "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { "
                  "let distance = length(position) - 1.0f; "
                  "return vec4<f32>(vec3<f32>(1.0f), distance); }"};
        }

        [[nodiscard]] compute::BoundsRequest makeBoundsRequest(
          std::uint64_t const sceneGeneration)
        {
            compute::BoundsRequest request;
            request.freshness = {.sceneGeneration = sceneGeneration,
                                 .viewGeneration = 3u,
                                 .parameterGeneration = 5u,
                                 .resourceGeneration = 11u,
                                 .evaluationGeneration = 13u};
            request.probeDomain = {.min = {-2.0f, -2.0f, -2.0f}, .max = {2.0f, 2.0f, 2.0f}};
            request.probeSettings.resolution = {16u, 16u, 16u};
            request.probeSettings.tileSize = {8u, 8u, 8u};
            return request;
        }

        /// @brief Session wired to the shared runtime context, exactly like an application window.
        [[nodiscard]] std::unique_ptr<compute::RenderBackendSession>
        makeSession(std::shared_ptr<webgpu::WebGPUComputeContext> const & context)
        {
            return std::make_unique<compute::RenderBackendSession>(
              std::make_unique<webgpu::WebGPUComputeRenderer>(context),
              std::make_shared<webgpu::WebGPUBoundsService>(context));
        }
    }

    /// @brief Scene invalidation writes through the queue while a probe encodes on another thread.
    ///
    /// Dawn keeps a single pending command buffer per queue, so an ungated WriteBuffer running
    /// concurrently with a compute pass opens two encoders on it and Metal aborts the process.
    TEST(WebGPUQueueSerialization, InvalidationWhileProbeEncoding_DoesNotRaceTheQueue)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
        if (!runtime->isAvailable())
        {
            GTEST_SKIP() << "WebGPU runtime unavailable: " << runtime->getErrorMessage();
        }
        auto context = runtime->getWebGPUContext();
        if (!context || !context->isValid())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        auto session = makeSession(context);
        ASSERT_TRUE(session->replaceScene(makeSphereSnapshot(7u), 5u)) << session->getErrorMessage();

        std::atomic<bool> done{false};
        std::atomic<int> invalidations{0};
        std::atomic<int> probesCompleted{0};
        std::string invalidationError;

        // Stands in for the design agent's render thread: GPU work driven off the UI thread.
        std::thread probeThread{[&]
        {
            while (!done.load(std::memory_order_acquire))
            {
                auto submission = session->getBoundsService()->submit(makeBoundsRequest(7u));
                submission->wait();
                auto result = submission->takeResult();
                if (result.has_value() && result->status == compute::BoundsResultStatus::Failed)
                {
                    invalidationError = "Bounds probe failed while the scene was invalidated";
                }
                ++probesCompleted;
            }
        }};

        // Stands in for the UI thread: every agent document or parameter edit invalidates the
        // precomputed SDF, which writes through the same queue.
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            if (!session->replaceScene(makeSphereSnapshot(7u), 5u))
            {
                invalidationError = session->getErrorMessage();
                break;
            }
            ++invalidations;
        }
        done.store(true, std::memory_order_release);
        probeThread.join();

        EXPECT_TRUE(invalidationError.empty()) << invalidationError;
        EXPECT_GT(invalidations.load(), 0);
        EXPECT_GT(probesCompleted.load(), 0);
        EXPECT_TRUE(context->isValid()) << context->getErrorMessage();
    }

    /// @brief Cancelling a stale prebake writes through the queue while another thread renders.
    TEST(WebGPUQueueSerialization, PrebakeCancellationWhileProbeEncoding_DoesNotRaceTheQueue)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
        if (!runtime->isAvailable())
        {
            GTEST_SKIP() << "WebGPU runtime unavailable: " << runtime->getErrorMessage();
        }
        auto context = runtime->getWebGPUContext();
        if (!context || !context->isValid())
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        auto session = makeSession(context);
        ASSERT_TRUE(session->replaceScene(makeSphereSnapshot(7u), 5u)) << session->getErrorMessage();

        std::atomic<bool> done{false};
        std::string failure;

        std::thread probeThread{[&]
        {
            while (!done.load(std::memory_order_acquire))
            {
                auto submission = session->getBoundsService()->submit(makeBoundsRequest(7u));
                submission->wait();
            }
        }};

        for (int iteration = 0; iteration < 8; ++iteration)
        {
            auto prebake = session->submitSdfPrecomputation(
              compute::RenderBounds{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, 8u, 5u);
            if (!prebake)
            {
                failure = session->getErrorMessage();
                break;
            }
            // Stale-submission cancellation, as NeutralFrameSubmissionTracker does on camera moves.
            prebake->requestCancellation();
            prebake->wait();
        }
        done.store(true, std::memory_order_release);
        probeThread.join();

        EXPECT_TRUE(failure.empty()) << failure;
        EXPECT_TRUE(context->isValid()) << context->getErrorMessage();
    }
}

#endif
