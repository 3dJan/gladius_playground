#if defined(GLADIUS_ENABLE_WEBGPU)

#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUProgramValidator.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

namespace gladius::tests
{
    TEST(WebGPUProgramValidator, ValidateShaderSource_WithNullContext_ReturnsUnavailableError)
    {
        webgpu::WebGPUProgramValidator validator(nullptr);
        EXPECT_FALSE(validator.isAvailable());

        auto const result = validator.validateShaderSource("@compute @workgroup_size(1) fn main() {}");
        EXPECT_FALSE(result.ok);
        EXPECT_FALSE(result.message.empty());
    }

    TEST(WebGPUProgramValidator, ValidateShaderSource_ValidAndInvalidWgsl_ReportsDiagnostics)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "Set GLADIUS_RUN_WEBGPU_TESTS=1 to run live WebGPU shader validation tests";
        }

        auto context = std::make_shared<webgpu::WebGPUComputeContext>();
        if (!context->isValid())
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << context->getErrorMessage();
        }

        webgpu::WebGPUProgramValidator validator(context);
        ASSERT_TRUE(validator.isAvailable());

        auto const validResult =
          validator.validateShaderSource("@compute @workgroup_size(1) fn main() {}");
        EXPECT_TRUE(validResult.ok) << validResult.message;

        auto const invalidResult =
          validator.validateShaderSource("@compute @workgroup_size(1) fn main() { let x = unknown_var; }");
        EXPECT_FALSE(invalidResult.ok);
        EXPECT_FALSE(invalidResult.message.empty());
    }
} // namespace gladius::tests

#endif
