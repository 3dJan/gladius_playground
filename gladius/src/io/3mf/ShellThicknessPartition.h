#pragma once

#include "FilamentOpticalProperties.h"

#include <cstddef>
#include <vector>

namespace gladius::io
{
    /// @brief Shell generation backend for shell-based color export.
    enum class ShellGenerationMode
    {
        LegacyManifoldDualContouring,
        OpenVdbColorThickness
    };

    /// @brief Depth interval for a single material shell measured inward from the visible surface.
    struct ShellLayerDepthInterval
    {
        std::size_t layerIndex = 0;
        float outerDepth = 0.0F;
        float innerDepth = 0.0F;

        [[nodiscard]] float thickness() const noexcept
        {
            return innerDepth - outerDepth;
        }

        [[nodiscard]] bool isEmpty(float epsilon = 1e-6F) const noexcept
        {
            return thickness() <= epsilon;
        }
    };

    /// @brief Helper for converting per-layer thicknesses into touching shell depth bands.
    ///
    /// Thicknesses are expected in filament stack order (bottom to top). The returned
    /// intervals are ordered from the visible outermost shell inward.
    class ShellThicknessPartition
    {
      public:
        /// @brief Build shell intervals from a thickness solution.
        [[nodiscard]] static std::vector<ShellLayerDepthInterval>
        buildIntervals(ThicknessSolution const& solution);

        /// @brief Build shell intervals from per-layer thicknesses in bottom-to-top order.
        [[nodiscard]] static std::vector<ShellLayerDepthInterval>
        buildIntervals(std::vector<float> const& thicknesses);

        /// @brief Compute the total occupied shell depth.
        [[nodiscard]] static float computeMaxDepth(std::vector<float> const& thicknesses);

      private:
        static void validateThicknesses(std::vector<float> const& thicknesses);
    };
} // namespace gladius::io
