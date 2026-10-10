#include "WebGPUDualContouringExporter.h"

#include "io/DualContouringMeshBuilder.h"
#include "io/IndexedTriangleMeshWriter.h"
#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "webgpu/WebGPUSdfEvaluationRequest.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::size_t MAX_WEBGPU_DUAL_CONTOUR_GRID_SAMPLES = 20'000'000u;

        [[nodiscard]] bool checkedProduct(std::size_t const left,
                                          std::size_t const right,
                                          std::size_t & result) noexcept
        {
            if (right != 0u && left > std::numeric_limits<std::size_t>::max() / right)
            {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] std::size_t calculateUniformDepth(std::size_t cellCount)
        {
            std::size_t depth = 0u;
            while (cellCount > 1u)
            {
                cellCount >>= 1u;
                ++depth;
            }
            return depth;
        }

        [[nodiscard]] dual_contouring::AxisAlignedBoundingBox makeGridBounds(
          compute::BoundsResult const & bounds,
          std::size_t const resolution)
        {
            auto const & modelBounds = *bounds.modelBounds;
            std::array<float, 3> extent{};
            float maxExtent = 0.0F;
            for (std::size_t axis = 0u; axis < 3u; ++axis)
            {
                extent[axis] = modelBounds.max[axis] - modelBounds.min[axis];
                maxExtent = std::max(maxExtent, extent[axis]);
            }

            auto const padding = bounds.errorBound +
                                 2.0F * maxExtent / static_cast<float>(resolution - 1u);
            auto const paddedBounds = modelBounds.expanded(padding);
            dual_contouring::AxisAlignedBoundingBox gridBounds{};
            gridBounds.min = Eigen::Vector3f{paddedBounds.min[0],
                                              paddedBounds.min[1],
                                              paddedBounds.min[2]};
            gridBounds.max = Eigen::Vector3f{paddedBounds.max[0],
                                              paddedBounds.max[1],
                                              paddedBounds.max[2]};
            if (!gridBounds.min.allFinite() || !gridBounds.max.allFinite() ||
                (gridBounds.max.array() <= gridBounds.min.array()).any())
            {
                throw std::runtime_error("Authoritative model bounds produced invalid SDF grid bounds");
            }
            return gridBounds;
        }
    }

    io::IndexedTriangleMesh WebGPUDualContouringExporter::generateMesh(
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      compute::BoundsResult const & bounds,
      io::DualContouringOptions const & options,
      WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        if (!context || !context->isValid())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        if (!snapshot.isValid())
        {
            throw std::invalid_argument("WebGPU dual contouring requires a valid analytic scene snapshot");
        }
        if (!bounds.isUsable())
        {
            throw std::invalid_argument("WebGPU dual contouring requires authoritative, unclipped model bounds");
        }
        if (options.sdfResolution < 2u)
        {
            throw std::invalid_argument("Dual contouring resolution must be at least 2");
        }
        if (!std::has_single_bit(options.sdfResolution - 1u))
        {
            throw std::invalid_argument(
              "WebGPU dual contouring requires resolution - 1 to be a power of two");
        }
        auto const uniformDepth = calculateUniformDepth(options.sdfResolution - 1u);
        if (uniformDepth > std::numeric_limits<std::uint8_t>::max())
        {
            throw std::length_error("Dual contouring resolution exceeds the supported octree depth");
        }
        if (!std::isfinite(options.isoValue))
        {
            throw std::invalid_argument("Dual contouring iso value must be finite");
        }

        std::size_t sampleCount = 0u;
        if (!checkedProduct(options.sdfResolution, options.sdfResolution, sampleCount) ||
            !checkedProduct(sampleCount, options.sdfResolution, sampleCount) ||
            sampleCount > MAX_WEBGPU_DUAL_CONTOUR_GRID_SAMPLES)
        {
            throw std::length_error(
              "WebGPU dual contouring resolution exceeds the bounded host-grid memory budget");
        }

        auto const gridBounds = makeGridBounds(bounds, options.sdfResolution);
        auto const spacing =
          (gridBounds.max - gridBounds.min) / static_cast<float>(options.sdfResolution - 1u);
        if (!spacing.allFinite() || (spacing.array() <= 0.0F).any())
        {
            throw std::runtime_error("WebGPU dual contouring produced invalid SDF grid spacing");
        }

        std::vector<float> samples(sampleCount);
        auto evaluationRequest = makeSdfEvaluationRequest(snapshot, options.isoValue);
        WebGPUSdfEvaluator evaluator{std::move(context)};
        auto const planeSize = options.sdfResolution * options.sdfResolution;
        bool const completed = evaluator.evaluateGenerated(
          evaluationRequest,
          sampleCount,
          [&](std::size_t const offset,
              std::size_t const count,
              std::vector<std::array<float, 3>> & positions)
          {
              positions.reserve(count);
              for (std::size_t index = offset; index < offset + count; ++index)
              {
                  auto const z = index / planeSize;
                  auto const y = (index / options.sdfResolution) % options.sdfResolution;
                  auto const x = index % options.sdfResolution;
                  Eigen::Vector3f const position =
                    gridBounds.min + spacing.cwiseProduct(Eigen::Vector3f{
                      static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
                  positions.push_back({position.x(), position.y(), position.z()});
              }
          },
          [&samples](std::size_t const offset, std::vector<float> && batchValues)
          {
              std::copy(batchValues.begin(),
                        batchValues.end(),
                        samples.begin() + static_cast<std::ptrdiff_t>(offset));
          },
          std::move(shouldCancel));
        if (!completed)
        {
            throw std::runtime_error("WebGPU dual contouring export was cancelled");
        }

        dual_contouring::OctreeBuildConfig config{};
        config.sdfResolution = options.sdfResolution;
        config.maxDepth = uniformDepth;
        config.isoValue = options.isoValue;
        // The current face emitter maps one leaf to one lattice cell. Uniform leaves are
        // therefore required for complete, consistently oriented closed topology.
        config.forceUniform = true;
        config.enableGpuSampling = false;
        config.enableCurvatureRefinement = false;
        config.enableBalancedRefinement = false;

        return io::buildIndexedDualContouringMesh(gridBounds,
                                                   config,
                                                   options.sdfResolution,
                                                   options.sdfResolution,
                                                   options.sdfResolution,
                                                   std::move(samples));
    }

    void WebGPUDualContouringExporter::exportStl(
      std::filesystem::path const & filePath,
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      compute::BoundsResult const & bounds,
      io::DualContouringOptions const & options,
      WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        auto mesh = generateMesh(std::move(context),
                                 snapshot,
                                 bounds,
                                 options,
                                 std::move(shouldCancel));
        io::writeIndexedMeshToBinaryStl(filePath, mesh);
    }

    void WebGPUDualContouringExporter::export3mf(
      std::filesystem::path const & filePath,
      std::shared_ptr<WebGPUComputeContext> context,
      compute::RenderSceneSnapshot const & snapshot,
      compute::BoundsResult const & bounds,
      io::DualContouringOptions const & options,
      WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        auto mesh = generateMesh(std::move(context),
                                 snapshot,
                                 bounds,
                                 options,
                                 std::move(shouldCancel));
        io::IndexedTriangleMesh3mfWriter writer;
        writer.exportMesh(filePath, mesh, filePath.stem().string());
    }
}
