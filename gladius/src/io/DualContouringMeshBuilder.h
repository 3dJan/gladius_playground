#pragma once

#include "IndexedTriangleMesh.h"
#include "../DualContouringMesher.h"

#include <cstddef>
#include <vector>

namespace gladius::io
{
    /// @brief Extract an indexed mesh from a backend-generated, x-major SDF lattice.
    [[nodiscard]] IndexedTriangleMesh buildIndexedDualContouringMesh(
      dual_contouring::AxisAlignedBoundingBox const & bounds,
      dual_contouring::OctreeBuildConfig const & config,
      std::size_t width,
      std::size_t height,
      std::size_t depth,
      std::vector<float> values,
      dual_contouring::DualContouringDiagnostics * diagnostics = nullptr);
}
