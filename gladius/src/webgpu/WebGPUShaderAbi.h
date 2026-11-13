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
        std::array<float, 4> forwardAndHorizontalScale{};
        std::array<float, 4> rightAndWidth{};
        std::array<float, 4> upAndHeight{};
        std::array<float, 4> verticalScaleAndMaxSteps{};
        std::array<float, 4> firstRowAndCount{};
        std::array<float, 4> timeSliceQualityNormal{};
        std::array<std::uint32_t, 4> flagsModeReserved{};
        std::array<float, 4> clippingBoxMin{};
        std::array<float, 4> clippingBoxMax{};
        /// x: reference thickness in mm, y: interior step size, z: max interior steps, w: transmittance floor.
        std::array<float, 4> translucencyParams{};
    };

    static_assert(alignof(FrameUniforms) == 16u);
    static_assert(offsetof(FrameUniforms, clippingBoxMin) == 128u);
    static_assert(offsetof(FrameUniforms, clippingBoxMax) == 144u);
    static_assert(offsetof(FrameUniforms, translucencyParams) == 160u);
    static_assert(sizeof(FrameUniforms) == 176u);

    /// Number of u32 ray-march metric counters exposed at frame shader binding 10.
    /// Indices 0..8 mirror compute::FrameMetrics; the rest are reserved padding.
    inline constexpr std::size_t FRAME_METRICS_COUNTER_COUNT = 16u;
    inline constexpr std::size_t FRAME_METRICS_SIZE_BYTES =
      FRAME_METRICS_COUNTER_COUNT * sizeof(std::uint32_t);

    /// FrameUniforms::flagsModeReserved.x bit that enables shader metric counters.
    /// Mirrors RF_DEBUG_METRICS in src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_DEBUG_METRICS = 32768u;
}