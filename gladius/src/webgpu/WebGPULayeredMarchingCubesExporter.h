#pragma once

#include "compute/BoundingBoxContracts.h"
#include "compute/RenderSceneSnapshot.h"
#include "io/IndexedTriangleMesh.h"
#include "io/MeshExportJob.h"
#include "webgpu/WebGPUSdfEvaluator.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace gladius::webgpu
{
    /// @brief Extract a dense uniform-grid isosurface using WebGPU SDF sampling.
    class WebGPULayeredMarchingCubesExporter final
    {
      public:
        using ProgressCallback = std::function<void(double, std::string_view)>;

        /// Generate the WebGPU equivalent of layered marching-cubes output.
        /// Quality levels map to bounded uniform-grid resolutions {65, 129, 193, 257}.
        [[nodiscard]] static io::IndexedTriangleMesh generateMesh(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          compute::BoundsResult const & bounds,
          std::size_t qualityLevel,
          io::MeshExportCancellationPredicate shouldCancel = {},
          ProgressCallback reportProgress = {});
    };
} // namespace gladius::webgpu
