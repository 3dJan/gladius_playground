#if defined(GLADIUS_ENABLE_WEBGPU)

#include "compute/BoundingBoxContracts.h"
#include "webgpu/WebGPUBoundsService.h"
#include "webgpu/WebGPUComputeContext.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

namespace gladius::tests
{
    namespace
    {
        [[nodiscard]] bool areWebGpuTestsEnabled()
        {
            if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] std::shared_ptr<const compute::RenderSceneSnapshot> makeSphereSnapshot(
          float const radius)
        {
            return std::make_shared<const compute::RenderSceneSnapshot>(compute::RenderSceneSnapshot{
              .sceneGeneration = 7u,
              .requiredCapabilities = compute::RendererCapability::AnalyticRendering,
              .analyticEvaluatorWgsl =
                "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { "
                "let distance = length(position) - " + std::to_string(radius) + "f; "
                "return vec4<f32>(vec3<f32>(1.0f), distance); }"});
        }

        [[nodiscard]] compute::BoundsRequest makeRequest()
        {
            compute::BoundsRequest request;
            request.freshness = {.sceneGeneration = 7u,
                                 .viewGeneration = 3u,
                                 .parameterGeneration = 5u,
                                 .resourceGeneration = 11u,
                                 .evaluationGeneration = 13u};
            request.probeDomain = {.min = {-2.0f, -2.0f, -2.0f}, .max = {2.0f, 2.0f, 2.0f}};
            request.probeSettings.resolution = {16u, 16u, 16u};
            request.probeSettings.tileSize = {8u, 8u, 8u};
            return request;
        }

        [[nodiscard]] std::unique_ptr<webgpu::WebGPUBoundsService> makeService()
        {
            try
            {
                auto context = std::make_shared<webgpu::WebGPUComputeContext>();
                if (!context->isValid())
                {
                    return nullptr;
                }
                return std::make_unique<webgpu::WebGPUBoundsService>(std::move(context));
            }
            catch (std::exception const &)
            {
                return nullptr;
            }
        }
    }

    TEST(WebGPUBoundsService, SphereSnapshot_ReturnsAuthoritativeModelBounds)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }
        auto service = makeService();
        if (!service)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        auto const snapshot = makeSphereSnapshot(1.0f);
        service->setSceneSnapshot(snapshot);
        auto const request = makeRequest();
        auto submission = service->submit(request);

        ASSERT_NE(submission, nullptr);
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::BoundsSubmissionStatus::Succeeded)
          << submission->getErrorMessage();
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->status, compute::BoundsResultStatus::Ready);
        ASSERT_TRUE(result->isUsable());
        ASSERT_TRUE(result->modelBounds.has_value());
        EXPECT_NEAR(result->modelBounds->min[0], -1.0f, 0.05f);
        EXPECT_NEAR(result->modelBounds->min[1], -1.0f, 0.05f);
        EXPECT_NEAR(result->modelBounds->min[2], -1.0f, 0.05f);
        EXPECT_NEAR(result->modelBounds->max[0], 1.0f, 0.05f);
        EXPECT_NEAR(result->modelBounds->max[1], 1.0f, 0.05f);
        EXPECT_NEAR(result->modelBounds->max[2], 1.0f, 0.05f);
        EXPECT_GT(result->diagnostics.probeCount, 0u);
        EXPECT_GT(result->diagnostics.projectedCount, 0u);
    }

    TEST(WebGPUBoundsService, CompletedResult_IsCachedOnlyForMatchingModelGeneration)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }
        auto service = makeService();
        if (!service)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        service->setSceneSnapshot(makeSphereSnapshot(1.0f));
        auto const request = makeRequest();
        auto submission = service->submit(request);
        ASSERT_NE(submission, nullptr);
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::BoundsSubmissionStatus::Succeeded);
        ASSERT_TRUE(submission->takeResult().has_value());

        EXPECT_TRUE(service->getCachedResult(request.freshness).has_value());

        auto viewChanged = request.freshness;
        viewChanged.viewGeneration++;
        EXPECT_TRUE(service->getCachedResult(viewChanged).has_value());

        auto parametersChanged = request.freshness;
        parametersChanged.parameterGeneration++;
        EXPECT_FALSE(service->getCachedResult(parametersChanged).has_value());
    }

    TEST(WebGPUBoundsService, PositiveDistanceField_ReturnsEmptyWithoutBuildVolumeFallback)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }
        auto service = makeService();
        if (!service)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        auto const snapshot = std::make_shared<const compute::RenderSceneSnapshot>(
          compute::RenderSceneSnapshot{
            .sceneGeneration = 7u,
            .requiredCapabilities = compute::RendererCapability::AnalyticRendering,
            .analyticEvaluatorWgsl =
              "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { "
              "return vec4<f32>(vec3<f32>(1.0f), 10.0f); }"});
        service->setSceneSnapshot(snapshot);
        auto request = makeRequest();
        request.probeDomain = {.min = {-1.0f, -1.0f, -1.0f}, .max = {1.0f, 1.0f, 1.0f}};

        auto submission = service->submit(request);
        ASSERT_NE(submission, nullptr);
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::BoundsSubmissionStatus::Succeeded);
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, compute::BoundsResultStatus::Empty);
        EXPECT_EQ(result->errorCode, compute::BoundsErrorCode::NoSurface);
        EXPECT_FALSE(result->modelBounds.has_value());
        EXPECT_FALSE(result->isUsable());
    }

    TEST(WebGPUBoundsService, SurfaceOutsideProbeDomain_ReturnsDomainClipped)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }
        auto service = makeService();
        if (!service)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        service->setSceneSnapshot(makeSphereSnapshot(2.0f));
        auto request = makeRequest();
        request.probeDomain = {.min = {-1.0f, -1.0f, -1.0f}, .max = {1.0f, 1.0f, 1.0f}};

        auto submission = service->submit(request);
        ASSERT_NE(submission, nullptr);
        submission->wait();
        ASSERT_EQ(submission->getStatus(), compute::BoundsSubmissionStatus::Succeeded);
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, compute::BoundsResultStatus::Failed);
        EXPECT_EQ(result->errorCode, compute::BoundsErrorCode::DomainClipped);
        EXPECT_TRUE(result->touchesProbeDomain);
        EXPECT_FALSE(result->isUsable());
    }

    TEST(WebGPUBoundsService, Cancellation_StopsBetweenProbeTiles)
    {
        if (!areWebGpuTestsEnabled())
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }
        auto service = makeService();
        if (!service)
        {
            GTEST_SKIP() << "WebGPU device unavailable";
        }

        service->setSceneSnapshot(makeSphereSnapshot(1.0f));
        auto request = makeRequest();
        request.probeSettings.resolution = {64u, 64u, 64u};
        request.probeSettings.tileSize = {4u, 4u, 4u};

        auto submission = service->submit(request);
        ASSERT_NE(submission, nullptr);
        submission->requestCancellation();
        submission->wait();

        ASSERT_EQ(submission->getStatus(), compute::BoundsSubmissionStatus::Cancelled);
        auto result = submission->takeResult();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, compute::BoundsResultStatus::Cancelled);
        EXPECT_EQ(result->errorCode, compute::BoundsErrorCode::Cancelled);
        EXPECT_FALSE(result->isUsable());
    }
}

#endif
