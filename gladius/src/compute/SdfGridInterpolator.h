#pragma once

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace gladius::compute
{
    /// @brief Samples a flattened, axis-aligned SDF grid with clamped trilinear interpolation.
    template <typename SampleValueAt>
    [[nodiscard]] float sampleTrilinearSdfGrid(Eigen::Vector3f const& position,
                                                Eigen::Vector3f const& boundsMin,
                                                Eigen::Vector3f const& boundsSize,
                                                std::size_t width,
                                                std::size_t height,
                                                std::size_t depth,
                                                SampleValueAt&& sampleValueAt)
    {
        if (width == 0U || height == 0U || depth == 0U)
        {
            return 0.0F;
        }

        Eigen::Vector3f const safeExtent = boundsSize.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
        Eigen::Vector3f normalized = (position - boundsMin).cwiseQuotient(safeExtent);
        normalized = normalized.cwiseMax(Eigen::Vector3f::Zero()).cwiseMin(Eigen::Vector3f::Ones());

        float const gx = normalized.x() * static_cast<float>(width - 1U);
        float const gy = normalized.y() * static_cast<float>(height - 1U);
        float const gz = normalized.z() * static_cast<float>(depth - 1U);
        std::size_t const x0 = static_cast<std::size_t>(std::floor(gx));
        std::size_t const y0 = static_cast<std::size_t>(std::floor(gy));
        std::size_t const z0 = static_cast<std::size_t>(std::floor(gz));
        std::size_t const x1 = std::min(x0 + 1U, width - 1U);
        std::size_t const y1 = std::min(y0 + 1U, height - 1U);
        std::size_t const z1 = std::min(z0 + 1U, depth - 1U);
        float const fx = gx - static_cast<float>(x0);
        float const fy = gy - static_cast<float>(y0);
        float const fz = gz - static_cast<float>(z0);

        auto const sampleAt = [&](std::size_t x, std::size_t y, std::size_t z)
        {
            std::size_t const index = z * width * height + y * width + x;
            return sampleValueAt(index);
        };

        float const v000 = sampleAt(x0, y0, z0);
        float const v100 = sampleAt(x1, y0, z0);
        float const v010 = sampleAt(x0, y1, z0);
        float const v110 = sampleAt(x1, y1, z0);
        float const v001 = sampleAt(x0, y0, z1);
        float const v101 = sampleAt(x1, y0, z1);
        float const v011 = sampleAt(x0, y1, z1);
        float const v111 = sampleAt(x1, y1, z1);

        float const v00 = v000 * (1.0F - fx) + v100 * fx;
        float const v01 = v001 * (1.0F - fx) + v101 * fx;
        float const v10 = v010 * (1.0F - fx) + v110 * fx;
        float const v11 = v011 * (1.0F - fx) + v111 * fx;
        float const v0 = v00 * (1.0F - fy) + v10 * fy;
        float const v1 = v01 * (1.0F - fy) + v11 * fy;
        return v0 * (1.0F - fz) + v1 * fz;
    }
} // namespace gladius::compute
