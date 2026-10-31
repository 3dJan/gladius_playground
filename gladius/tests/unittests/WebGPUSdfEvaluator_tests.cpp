#if defined(GLADIUS_ENABLE_WEBGPU)

#include "compute/SdfEvaluation.h"
#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace gladius::tests
{
    TEST(WebGPUSdfEvaluator, EvaluateGeneratedAsync_CancelledBeforeStart_DoesNotAccessDeviceOrCallbacks)
    {
        webgpu::WebGPUSdfEvaluator evaluator(nullptr);
        int cancellationChecks = 0;
        int sourceCalls = 0;
        int sinkCalls = 0;
        auto task = evaluator.evaluateGeneratedAsync(
          {},
          1u,
          [&sourceCalls](std::size_t, std::size_t, std::vector<std::array<float, 3>> &)
          { ++sourceCalls; },
          [&sinkCalls](std::size_t, std::vector<float> &&) { ++sinkCalls; },
          [&cancellationChecks]
          {
              ++cancellationChecks;
              return true;
          });

        EXPECT_EQ(cancellationChecks, 0);
        EXPECT_EQ(sourceCalls, 0);
        EXPECT_EQ(sinkCalls, 0);
        EXPECT_FALSE(task.done());

        task.step();

        ASSERT_TRUE(task.done());
        EXPECT_FALSE(task.take());
        EXPECT_EQ(cancellationChecks, 1);
        EXPECT_EQ(sourceCalls, 0);
        EXPECT_EQ(sinkCalls, 0);
    }

    TEST(WebGPUSdfEvaluator, EvaluateGeneratedAsync_EmptyInput_CompletesWithoutDevice)
    {
        webgpu::WebGPUSdfEvaluator evaluator(nullptr);
        auto task = evaluator.evaluateGeneratedAsync({}, 0u, {}, {});

        EXPECT_FALSE(task.done());
        task.step();

        ASSERT_TRUE(task.done());
        EXPECT_TRUE(task.take());
    }

    TEST(WebGPUSdfEvaluator, EvaluateColorsGeneratedAsync_CancelledBeforeStart_DoesNotAccessDevice)
    {
        webgpu::WebGPUSdfEvaluator evaluator(nullptr);
        compute::SdfEvaluationRequest request;
        request.returnColor = true;
        int cancellationChecks = 0;
        int sinkCalls = 0;
        auto task = evaluator.evaluateColorsGeneratedAsync(
          std::move(request),
          1u,
          {},
          [&sinkCalls](std::size_t, std::vector<std::array<float, 3>> &&) { ++sinkCalls; },
          [&cancellationChecks]
          {
              ++cancellationChecks;
              return true;
          });

        EXPECT_EQ(cancellationChecks, 0);
        task.step();

        ASSERT_TRUE(task.done());
        EXPECT_FALSE(task.take());
        EXPECT_EQ(cancellationChecks, 1);
        EXPECT_EQ(sinkCalls, 0);
    }

    TEST(WebGPUSdfEvaluator, ReturnsRawFloatValuesForNonMultiplePointCount)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUSdfEvaluator> evaluator;
        try
        {
            evaluator = std::make_unique<webgpu::WebGPUSdfEvaluator>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(evaluator->isAvailable()) << evaluator->getErrorMessage();
        auto const result = evaluator->evaluate(compute::SdfEvaluationRequest{
          .positions = {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}},
          .isoValue = 0.25f,
          .shaderSource = webgpu::WebGPUSdfShaderComposer::compose(R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    return vec4<f32>(vec3<f32>(0.0), length(position));
}
)"),
          .parameterValues = {}});

        ASSERT_EQ(result.values.size(), 3u);
        EXPECT_NEAR(result.values[0], 0.75f, 1.0e-6f);
        EXPECT_NEAR(result.values[1], -0.25f, 1.0e-6f);
        EXPECT_NEAR(result.values[2], 1.75f, 1.0e-6f);
        for (auto const value : result.values)
        {
            EXPECT_TRUE(std::isfinite(value));
        }
    }

    TEST(WebGPUSdfEvaluator, ReturnsModelColorsForGeneratedPositions)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<webgpu::WebGPUSdfEvaluator> evaluator;
        try
        {
            evaluator = std::make_unique<webgpu::WebGPUSdfEvaluator>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }

        ASSERT_TRUE(evaluator->isAvailable()) << evaluator->getErrorMessage();
        compute::SdfEvaluationRequest request;
        request.returnColor = true;
        request.shaderSource = webgpu::WebGPUSdfShaderComposer::composeColorWithResourceSupport(
          R"(
fn evaluateModel(position: vec3<f32>) -> vec4<f32> {
    return vec4<f32>(position * 0.5 + vec3<f32>(0.5), length(position));
}
)",
          false,
          false,
          false);

        std::vector<std::array<float, 3>> colors;
        bool const completed = evaluator->evaluateColorsGenerated(
          request,
          3u,
          [](std::size_t const offset,
             std::size_t const count,
             std::vector<std::array<float, 3>> & positions)
          {
              std::vector<std::array<float, 3>> const allPositions{
                {-1.0F, 0.0F, 1.0F}, {0.0F, 0.5F, -0.5F}, {1.0F, -1.0F, 0.0F}};
              positions.assign(allPositions.begin() + static_cast<std::ptrdiff_t>(offset),
                               allPositions.begin() + static_cast<std::ptrdiff_t>(offset + count));
          },
          [&colors](std::size_t const offset, std::vector<std::array<float, 3>> && batch)
          {
              if (offset == 0u)
              {
                  colors = std::move(batch);
              }
          });

        ASSERT_TRUE(completed);
        ASSERT_EQ(colors.size(), 3u);
        EXPECT_NEAR(colors[0][0], 0.0F, 1.0e-6F);
        EXPECT_NEAR(colors[0][1], 0.5F, 1.0e-6F);
        EXPECT_NEAR(colors[0][2], 1.0F, 1.0e-6F);
        EXPECT_NEAR(colors[1][0], 0.5F, 1.0e-6F);
        EXPECT_NEAR(colors[1][1], 0.75F, 1.0e-6F);
        EXPECT_NEAR(colors[1][2], 0.25F, 1.0e-6F);
    }
}

#endif
