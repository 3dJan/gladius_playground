#pragma once

#include <array>
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

    /**
     * @brief Camera data shared with the headless WebGPU frame ray marcher.
     */
    struct alignas(16) FrameUniforms
    {
        std::array<float, 4> eyeAndMaxDistance{};
        std::array<float, 4> forwardAndFieldOfView{};
        std::array<float, 4> rightAndWidth{};
        std::array<float, 4> upAndHeight{};
    };

    static_assert(alignof(FrameUniforms) == 16u);
    static_assert(sizeof(FrameUniforms) == 64u);
}