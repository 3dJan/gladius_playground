#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gladius::webgpu
{
    inline constexpr std::uint32_t SLICE_WORKGROUP_WIDTH = 16u;
    inline constexpr std::uint32_t SLICE_WORKGROUP_HEIGHT = 16u;
    inline constexpr std::size_t PACKED_PIXEL_SIZE_BYTES = sizeof(std::uint32_t);

    struct SliceDispatchSize
    {
        std::uint32_t workgroupsX{};
        std::uint32_t workgroupsY{};
        std::size_t outputSizeBytes{};
    };

    /**
     * @brief Calculates a safe 16x16 workgroup dispatch and packed output size.
     */
    [[nodiscard]] constexpr std::optional<SliceDispatchSize>
    calculateSliceDispatchSize(std::uint32_t width, std::uint32_t height) noexcept
    {
        if (width == 0u || height == 0u)
        {
            return std::nullopt;
        }

        auto const pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (pixelCount / width != height || pixelCount > (SIZE_MAX / PACKED_PIXEL_SIZE_BYTES))
        {
            return std::nullopt;
        }

        return SliceDispatchSize{
          .workgroupsX = ((width - 1u) / SLICE_WORKGROUP_WIDTH) + 1u,
          .workgroupsY = ((height - 1u) / SLICE_WORKGROUP_HEIGHT) + 1u,
          .outputSizeBytes = pixelCount * PACKED_PIXEL_SIZE_BYTES};
    }
}