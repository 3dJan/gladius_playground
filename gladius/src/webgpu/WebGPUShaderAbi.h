#pragma once

#include <cstddef>
#include <cstdint>

namespace gladius::webgpu
{
    /**
     * @brief Uniform data shared with the 2D WebGPU slice compute shader.
     */
    struct alignas(16) SliceUniforms
    {
        float sliceZ{};
        std::uint32_t width{};
        std::uint32_t height{};
        float scale{};
    };

    static_assert(alignof(SliceUniforms) == 16u);
    static_assert(sizeof(SliceUniforms) == 16u);
    static_assert(offsetof(SliceUniforms, sliceZ) == 0u);
    static_assert(offsetof(SliceUniforms, width) == 4u);
    static_assert(offsetof(SliceUniforms, height) == 8u);
    static_assert(offsetof(SliceUniforms, scale) == 12u);
}