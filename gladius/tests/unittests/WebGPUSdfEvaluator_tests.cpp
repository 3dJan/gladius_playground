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

namespace gladius::tests
{
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
}

#endif
