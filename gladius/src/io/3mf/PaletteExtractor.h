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
        ManifoldDualContouringOptions manifoldOptions{};
        bool convertToSrgb{true};
    };

    /// Derive a palette from per-vertex colors of a mesh generated with manifold dual contouring
    /// using the provided compute core and options. Throws on failure.
    std::vector<Eigen::Vector3f> derivePaletteFromMesh(gladius::ComputeCore & core,
                                                       PaletteExtractionOptions const & options);
}
