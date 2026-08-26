#pragma once

#include "Contour.h"
#include "EventLogger.h"
#include "ImageRGBA.h"
#include "kernel/types.h"

#include <vector>

namespace gladius::slicer
{
    /// A dense 2D grid of signed distance samples covering a clipping area.
    ///
    /// Values are stored row-major with node (x, y) located at
    /// clippingArea.xy + cellSize * (x, y) where
    /// cellSize = (clippingArea.zw - clippingArea.xy) / (dim - 1).
    /// This matches the sampling convention of the OpenCL slicer kernels.
    struct SdfGrid
    {
        std::vector<float> values;
        int width{};
        int height{};
        float4 clippingArea{};

        [[nodiscard]] float cellSizeX() const
        {
            return (clippingArea.z - clippingArea.x) / static_cast<float>(width - 1);
        }

        [[nodiscard]] float cellSizeY() const
        {
            return (clippingArea.w - clippingArea.y) / static_cast<float>(height - 1);
        }

        /// Bilinearly interpolated distance at an arbitrary position inside the clip area.
        [[nodiscard]] float sample(Eigen::Vector2f const & pos) const;
    };

    /**
     * @brief Backend-neutral contour extraction from a dense SDF grid.
     *
     * Mirrors the two contour paths of ComputeCore (marching squares and adaptive
     * quadtree) so that any backend able to produce an SdfGrid (OpenCL, WebGPU,
     * CPU reference) yields comparable polylines through identical host code.
     */
    namespace GridContourBuilder
    {
        /// Port of the OpenCL computeMarchingSquareStates kernel semantics:
        /// bit 1 = sample(x-1, y-1) < 0, bit 2 = sample(x, y-1) < 0,
        /// bit 4 = sample(x-1, y) < 0, bit 8 = sample(x, y) < 0.
        [[nodiscard]] MarchingSquaresStates buildMarchingSquareStates(
          ComputeContext & context, SdfGrid const & grid);

        /// Dense path: marching squares states + ContourExtractor post processing.
        [[nodiscard]] PolyLines extractDenseContours(SdfGrid const & grid,
                                                     ComputeContext & context,
                                                     events::SharedLogger const & logger);

        /// Adaptive path: Morton quadtree refinement over a bilinear sampler of the grid.
        [[nodiscard]] PolyLines extractAdaptiveContours(SdfGrid const & grid,
                                                        float minFeatureSize_mm,
                                                        events::SharedLogger const & logger);
    }
}
