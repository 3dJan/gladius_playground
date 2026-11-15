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

    /// FrameUniforms::flagsModeReserved.x bit that skips marching for rays provably
    /// missing the scene bounds. Mirrors RF_SKIP_BACKGROUND_RAYS in src/kernel/types.h
    /// and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_SKIP_BACKGROUND_RAYS = 32u;

    /// FrameUniforms::flagsModeReserved.x bit that coarsens the soft-shadow march.
    /// Mirrors RF_FAST_SHADOW in src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_FAST_SHADOW = 64u;

    /// FrameUniforms::flagsModeReserved.x bit that reduces AO to 4 taps.
    /// Mirrors RF_FAST_AO in src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_FAST_AO = 128u;

    /// FrameUniforms::flagsModeReserved.x bit that skips whole workgroup tiles whose
    /// frustum box provably misses the scene bounds. Mirrors RF_TILE_CULLING in
    /// src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_TILE_CULLING = 256u;

    /// FrameUniforms::flagsModeReserved.x bit that coarsens surface hit refinement.
    /// Mirrors RF_COARSE_REFINEMENT in src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_COARSE_REFINEMENT = 512u;

    /// FrameUniforms::flagsModeReserved.x bit that enables prebaked 3D SDF empty-space skipping.
    /// Mirrors RF_USE_PRECOMPUTED_SDF in src/kernel/types.h and frame_compute.wgsl.
    inline constexpr std::uint32_t RF_USE_PRECOMPUTED_SDF = 1024u;

    /**
     * @brief Metadata for the optional prebaked 3D SDF texture (frame binding 11, prebake binding 0).
     */
    struct alignas(16) PrecomputedSdfUniforms
    {
        std::array<float, 4> bboxMinAndTolerance{};
        std::array<float, 4> bboxMaxAndValid{};
        std::array<std::uint32_t, 4> gridDims{};
    };

    static_assert(alignof(PrecomputedSdfUniforms) == 16u);
    static_assert(sizeof(PrecomputedSdfUniforms) == 48u);
}