#pragma once

#include "compute/RenderContracts.h"

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

    namespace detail
    {
        /// @brief Shared dispatch arithmetic for a given workgroup tile shape.
        [[nodiscard]] constexpr std::optional<SliceDispatchSize>
        calculateDispatchSize(std::uint32_t width, std::uint32_t height,
                              std::uint32_t workgroupWidth, std::uint32_t workgroupHeight) noexcept
        {
            if (width == 0u || height == 0u || workgroupWidth == 0u || workgroupHeight == 0u)
            {
                return std::nullopt;
            }

            auto const pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            if (pixelCount / width != height || pixelCount > (SIZE_MAX / PACKED_PIXEL_SIZE_BYTES))
            {
                return std::nullopt;
            }

            return SliceDispatchSize{
              .workgroupsX = ((width - 1u) / workgroupWidth) + 1u,
              .workgroupsY = ((height - 1u) / workgroupHeight) + 1u,
              .outputSizeBytes = pixelCount * PACKED_PIXEL_SIZE_BYTES};
        }
    }

    /**
     * @brief Calculates a safe 16x16 workgroup dispatch and packed output size.
     */
    [[nodiscard]] constexpr std::optional<SliceDispatchSize>
    calculateSliceDispatchSize(std::uint32_t width, std::uint32_t height) noexcept
    {
        return detail::calculateDispatchSize(width, height, SLICE_WORKGROUP_WIDTH, SLICE_WORKGROUP_HEIGHT);
    }

    /**
     * @brief Calculates the dispatch for an explicit workgroup tile shape.
     *
     * The packed output size is independent of the tile shape; only the workgroup counts change.
     */
    [[nodiscard]] constexpr std::optional<SliceDispatchSize>
    calculateSliceDispatchSize(std::uint32_t width, std::uint32_t height,
                               compute::WorkgroupShape const shape) noexcept
    {
        if (!shape.isValid())
        {
            return std::nullopt;
        }
        return detail::calculateDispatchSize(width, height, shape.width, shape.height);
    }
}