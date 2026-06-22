#include "ShellThicknessPartition.h"

#include <cmath>
#include <stdexcept>

namespace gladius::io
{
    std::vector<ShellLayerDepthInterval>
    ShellThicknessPartition::buildIntervals(ThicknessSolution const& solution)
    {
        return buildIntervals(solution.thicknesses);
    }

    std::vector<ShellLayerDepthInterval>
    ShellThicknessPartition::buildIntervals(std::vector<float> const& thicknesses)
    {
        validateThicknesses(thicknesses);

        std::vector<ShellLayerDepthInterval> intervals;
        intervals.reserve(thicknesses.size());

        float cumulativeDepth = 0.0F;
        for (std::size_t reverseIndex = thicknesses.size(); reverseIndex > 0; --reverseIndex)
        {
            std::size_t const layerIndex = reverseIndex - 1;
            float const thickness = thicknesses[layerIndex];
            if (thickness <= 0.0F)
            {
                continue;
            }

            ShellLayerDepthInterval interval;
            interval.layerIndex = layerIndex;
            interval.outerDepth = cumulativeDepth;
            interval.innerDepth = cumulativeDepth + thickness;
            intervals.push_back(interval);

            cumulativeDepth = interval.innerDepth;
        }

        return intervals;
    }

    float ShellThicknessPartition::computeMaxDepth(std::vector<float> const& thicknesses)
    {
        validateThicknesses(thicknesses);

        float totalDepth = 0.0F;
        for (float const thickness : thicknesses)
        {
            if (thickness > 0.0F)
            {
                totalDepth += thickness;
            }
        }

        return totalDepth;
    }

    void ShellThicknessPartition::validateThicknesses(std::vector<float> const& thicknesses)
    {
        for (float const thickness : thicknesses)
        {
            if (!std::isfinite(thickness))
            {
                throw std::runtime_error("Shell thickness must be finite");
            }

            if (thickness < 0.0F)
            {
                throw std::runtime_error("Shell thickness must not be negative");
            }
        }
    }
} // namespace gladius::io