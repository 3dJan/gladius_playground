#pragma once

#include "compute/AsyncTask.h"
#include "compute/BoundingBoxContracts.h"
#include "compute/RenderSceneSnapshot.h"
#include "io/IndexedTriangleMesh.h"
#include "io/SurfaceExtractionOptions.h"
#include "webgpu/WebGPUSdfEvaluator.h"

#include <filesystem>
#include <memory>

namespace gladius::webgpu
{
    /// @brief Samples an analytic scene on WebGPU and extracts a validated host dual-contouring mesh.
    class WebGPUDualContouringExporter final
    {
      public:
        [[nodiscard]] static io::IndexedTriangleMesh generateMesh(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          compute::BoundsResult const & bounds,
          io::DualContouringOptions const & options,
          WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});

        /// @brief Coroutine variant of generateMesh. Suspends during GPU readbacks instead of blocking.
        [[nodiscard]] static compute::AsyncTask<io::IndexedTriangleMesh> generateMeshAsync(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          compute::BoundsResult bounds,
          io::DualContouringOptions options,
          WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});

        static void exportStl(std::filesystem::path const & filePath,
                              std::shared_ptr<WebGPUComputeContext> context,
                              compute::RenderSceneSnapshot const & snapshot,
                              compute::BoundsResult const & bounds,
                              io::DualContouringOptions const & options,
                              WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});

        static void export3mf(std::filesystem::path const & filePath,
                    std::shared_ptr<WebGPUComputeContext> context,
                    compute::RenderSceneSnapshot const & snapshot,
                    compute::BoundsResult const & bounds,
                    io::DualContouringOptions const & options,
                    WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});
    };
}
