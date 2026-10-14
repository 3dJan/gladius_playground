#include "webgpu/WebGPUBufferCapacity.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

namespace gladius::webgpu::tests
{
    TEST(WebGPUBufferCapacity, ShrinkingRequest_RetainsExistingCapacity)
    {
        EXPECT_EQ(detail::growBufferCapacity(4096u, 1024u), 4096u);
        EXPECT_EQ(detail::growBufferCapacity(4096u, 4096u), 4096u);
    }

    TEST(WebGPUBufferCapacity, GrowingRequest_AddsGeometricHeadroom)
    {
        EXPECT_EQ(detail::growBufferCapacity(1024u, 1025u), 1536u);
    }

    TEST(WebGPUBufferCapacity, Growth_IsAlignedForTypedGpuBuffers)
    {
        EXPECT_EQ(detail::growBufferCapacity(10u, 11u), 16u);
    }

    TEST(WebGPUBufferCapacity, NearSizeLimit_FallsBackToRequiredCapacity)
    {
        auto constexpr maximum = std::numeric_limits<std::size_t>::max();
        EXPECT_EQ(detail::growBufferCapacity(maximum - 3u, maximum), maximum);
    }
}
