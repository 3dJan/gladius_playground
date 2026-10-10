#pragma once

#include "compute/BoundingBoxContracts.h"
#include "compute/RenderSceneSnapshot.h"
#include "io/IndexedTriangleMesh.h"
#include "io/SurfaceExtractionOptions.h"
#include "webgpu/WebGPUSdfEvaluator.h"

#include <filesystem>
#include <memory>

namespace gladius::webgpu
{
    /// @brief Extracts a manifold mesh through the shared host octree and WebGPU SDF queries.
    class WebGPUManifoldDualContouringExporter final
    {
      public:
        [[nodiscard]] static io::IndexedTriangleMesh generateMesh(
            std::shared_ptr<WebGPUComputeContext> context,
            compute::RenderSceneSnapshot const& snapshot,
            compute::BoundsResult const& bounds,
            io::ManifoldDualContouringOptions const& options,
            WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});

        static void exportStl(
            std::filesystem::path const& filePath,
            std::shared_ptr<WebGPUComputeContext> context,
            compute::RenderSceneSnapshot const& snapshot,
            compute::BoundsResult const& bounds,
            io::ManifoldDualContouringOptions const& options,
            WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});

        static void export3mf(
            std::filesystem::path const& filePath,
            std::shared_ptr<WebGPUComputeContext> context,
            compute::RenderSceneSnapshot const& snapshot,
            compute::BoundsResult const& bounds,
            io::ManifoldDualContouringOptions const& options,
            WebGPUSdfEvaluator::CancellationPredicate shouldCancel = {});
    };
} // namespace gladius::webgpu
