#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gladius::webgpu::detail
{
    [[nodiscard]] constexpr std::size_t growBufferCapacity(std::size_t currentCapacity,
                                                            std::size_t requiredCapacity) noexcept
    {
        if (requiredCapacity <= currentCapacity)
        {
            return currentCapacity;
        }
        if (currentCapacity == 0u)
        {
            return requiredCapacity;
        }

        constexpr std::size_t alignment = 4u;
        auto const maximum = std::numeric_limits<std::size_t>::max();
        auto const growth = std::max(alignment, currentCapacity / 2u);
        if (currentCapacity > maximum - growth || currentCapacity + growth > maximum - (alignment - 1u))
        {
            return requiredCapacity;
        }

        auto const grownCapacity = (currentCapacity + growth + alignment - 1u) & ~(alignment - 1u);
        return std::max(requiredCapacity, grownCapacity);
    }
}
