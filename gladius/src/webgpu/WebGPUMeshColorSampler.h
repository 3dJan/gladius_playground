#pragma once

#include "compute/AsyncTask.h"
#include "compute/RenderSceneSnapshot.h"
#include "io/3mf/FaceColors.h"
#include "io/IndexedTriangleMesh.h"
#include "io/MeshExportJob.h"
#include "webgpu/WebGPUComputeContext.h"

#include <functional>
#include <memory>
#include <vector>

namespace gladius::webgpu
{
    /// @brief Samples model color output on WebGPU at indexed mesh face locations.
    class WebGPUMeshColorSampler final
    {
      public:
        using ProgressCallback = std::function<void(double)>;

        [[nodiscard]] static io::FaceColors sampleFaceColors(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          io::IndexedTriangleMesh const & mesh,
          bool convertToSrgb,
          io::MeshExportCancellationPredicate shouldCancel = {},
          ProgressCallback reportProgress = {});

        [[nodiscard]] static std::vector<io::FaceColors> sampleFaceColorsMultipoint(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          io::IndexedTriangleMesh const & mesh,
          bool convertToSrgb,
          io::MeshExportCancellationPredicate shouldCancel = {},
          ProgressCallback reportProgress = {});

        /// @brief Coroutine variant of sampleFaceColors. The mesh and snapshot must outlive the returned task.
        [[nodiscard]] static compute::AsyncTask<io::FaceColors> sampleFaceColorsAsync(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          io::IndexedTriangleMesh const & mesh,
          bool convertToSrgb,
          io::MeshExportCancellationPredicate shouldCancel = {},
          ProgressCallback reportProgress = {});

        /// @brief Coroutine variant of sampleFaceColorsMultipoint. The mesh and snapshot must outlive the returned task.
        [[nodiscard]] static compute::AsyncTask<std::vector<io::FaceColors>> sampleFaceColorsMultipointAsync(
          std::shared_ptr<WebGPUComputeContext> context,
          compute::RenderSceneSnapshot const & snapshot,
          io::IndexedTriangleMesh const & mesh,
          bool convertToSrgb,
          io::MeshExportCancellationPredicate shouldCancel = {},
          ProgressCallback reportProgress = {});
    };
} // namespace gladius::webgpu
