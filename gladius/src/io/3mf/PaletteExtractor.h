#pragma once

#include "io/SurfaceExtractionOptions.h"

#include <Eigen/Core>

#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    struct PaletteExtractionOptions
    {
        SurfaceExtractionMethod method{SurfaceExtractionMethod::ManifoldDualContouring};
        std::size_t marchingCubesQualityLevel{1U};
        ManifoldDualContouringOptions manifoldOptions{};
        bool convertToSrgb{true};
    };

    /// Derive a palette from per-vertex colors of a mesh generated with the selected
    /// surface extraction method using the provided compute core and options. Throws on failure.
    std::vector<Eigen::Vector3f> derivePaletteFromMesh(gladius::ComputeCore & core,
                                                       PaletteExtractionOptions const & options);
}
