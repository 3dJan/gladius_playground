#include "WebGPUManifoldDualContouringExporter.h"

#include "WebGPUManifoldSdfSampler.h"
#include "compute/FastQemSimplification.h"
#include "compute/GlobalMortonOctree.h"
#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "io/IndexedTriangleMeshWriter.h"
#include "io/WindingRepair.h"
#include "webgpu/WebGPUSdfEvaluationRequest.h"

#include "types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gladius::webgpu
{
    namespace
    {
        constexpr std::size_t kMaximumManifoldDepth = 12U;
        constexpr io::IndexedTriangleMeshValidationPolicy kWebGpuOutputValidationPolicy =
            io::IndexedTriangleMeshValidationPolicy::AllowGeometricDegenerates;
        constexpr std::size_t kSurfaceAdoptionIterations = 100U;
        constexpr float kSurfaceNormalEpsilon = 1.0e-4F;
        constexpr float kSurfaceAdoptionStepScale = 0.5F;

        [[nodiscard]] bool isCancelled(WebGPUSdfEvaluator::CancellationPredicate const& shouldCancel)
        {
            return shouldCancel && shouldCancel();
        }

        void throwIfCancelled(WebGPUSdfEvaluator::CancellationPredicate const& shouldCancel)
        {
            if (isCancelled(shouldCancel))
            {
                throw std::runtime_error("WebGPU manifold mesh export was cancelled");
            }
        }

        void requireCompleted(compute::ManifoldSdfSamplingStatus const status,
                              WebGPUSdfEvaluator::CancellationPredicate const& shouldCancel)
        {
            if (status == compute::ManifoldSdfSamplingStatus::Completed)
            {
                return;
            }
            if (status == compute::ManifoldSdfSamplingStatus::Cancelled || isCancelled(shouldCancel))
            {
                throw std::runtime_error("WebGPU manifold mesh export was cancelled");
            }
            throw std::runtime_error("WebGPU manifold SDF sampling is unavailable or incomplete"); 
        }

        void setPaddedBounds(
            compute::GlobalMortonOctreeConfig& config,
            compute::BoundsResult const& bounds,
            std::size_t const maxDepth)
        {
            auto const& modelBounds = *bounds.modelBounds;
            Eigen::Vector3f const min(
                modelBounds.min[0], modelBounds.min[1], modelBounds.min[2]);
            Eigen::Vector3f const max(
                modelBounds.max[0], modelBounds.max[1], modelBounds.max[2]);
            Eigen::Vector3f const size = max - min;
            float const maxExtent = size.maxCoeff();
            float const margin = 2.0F * maxExtent / static_cast<float>(1U << maxDepth);
            Eigen::Vector3f const paddedMin = min - Eigen::Vector3f::Constant(margin);
            Eigen::Vector3f const paddedMax = max + Eigen::Vector3f::Constant(margin);

            config.boundingBoxOverride.emplace();
            auto& result = *config.boundingBoxOverride;
            result.min.s[0] = paddedMin.x();
            result.min.s[1] = paddedMin.y();
            result.min.s[2] = paddedMin.z();
            result.min.s[3] = 0.0F;
            result.max.s[0] = paddedMax.x();
            result.max.s[1] = paddedMax.y();
            result.max.s[2] = paddedMax.z();
            result.max.s[3] = 0.0F;
        }

        void adoptVerticesToSurface(
            WebGPUManifoldSdfSampler const& sampler,
            std::vector<Eigen::Vector3f>& positions,
            WebGPUSdfEvaluator::CancellationPredicate const& shouldCancel)
        {
            if (positions.empty())
            {
                return;
            }
            if (positions.size() > std::numeric_limits<std::size_t>::max() / 5U)
            {
                throw std::length_error("WebGPU manifold vertex projection batch is too large");
            }

            // Match SlicerProgram::adoptVertexOfMeshToSurface: normalized tetrahedral normals,
            // a half-distance step clamped to one unit, and the best iterate retained per vertex.
            std::array<Eigen::Vector3f, 4> const normalOffsets = {
                Eigen::Vector3f(1.0F, -1.0F, -1.0F) * 0.5773F,
                Eigen::Vector3f(-1.0F, -1.0F, 1.0F) * 0.5773F,
                Eigen::Vector3f(-1.0F, 1.0F, -1.0F) * 0.5773F,
                Eigen::Vector3f(1.0F, 1.0F, 1.0F) * 0.5773F};

            std::vector<Eigen::Vector3f> bestPositions = positions;
            std::vector<float> minDistances(positions.size(), std::numeric_limits<float>::max());
            std::vector<std::uint8_t> converged(positions.size(), 0U);
            std::vector<Eigen::Vector3f> queryPositions(positions.size() * 5U);
            std::vector<float> values;
            compute::ManifoldSdfSamplingCallbacks callbacks;
            callbacks.shouldCancel = shouldCancel;

            for (std::size_t iteration = 0U; iteration < kSurfaceAdoptionIterations; ++iteration)
            {
                throwIfCancelled(shouldCancel);
                for (std::size_t index = 0U; index < positions.size(); ++index)
                {
                    std::size_t const base = index * 5U;
                    queryPositions[base] = positions[index];
                    for (std::size_t offsetIndex = 0U; offsetIndex < normalOffsets.size(); ++offsetIndex)
                    {
                        queryPositions[base + offsetIndex + 1U] =
                            positions[index] + normalOffsets[offsetIndex] * kSurfaceNormalEpsilon;
                    }
                }

                requireCompleted(
                    sampler.evaluateSdfBatch(queryPositions, 0.0F, values, callbacks),
                    shouldCancel);
                if (values.size() != queryPositions.size())
                {
                    throw std::runtime_error("WebGPU manifold vertex projection returned incomplete data");
                }

                for (std::size_t index = 0U; index < positions.size(); ++index)
                {
                    if (converged[index] != 0U)
                    {
                        continue;
                    }
                    std::size_t const base = index * 5U;
                    float const sdf = values[base];
                    float const absSdf = std::abs(sdf);
                    if (absSdf < minDistances[index])
                    {
                        minDistances[index] = absSdf;
                        bestPositions[index] = positions[index];
                    }

                    if (absSdf <= 1.0e-5F)
                    {
                        positions[index] = bestPositions[index];
                        converged[index] = 1U;
                        continue;
                    }

                    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
                    for (std::size_t offsetIndex = 0U; offsetIndex < normalOffsets.size(); ++offsetIndex)
                    {
                        normal += normalOffsets[offsetIndex] * values[base + offsetIndex + 1U];
                    }
                    float const normalLength = normal.norm();
                    if (normalLength > 0.0F)
                    {
                        normal /= normalLength;
                    }

                    float const step = std::clamp(sdf, -1.0F, 1.0F) * kSurfaceAdoptionStepScale;
                    positions[index] -= normal * step;
                }
            }

            positions = std::move(bestPositions);
        }
    } // namespace

    io::IndexedTriangleMesh WebGPUManifoldDualContouringExporter::generateMesh(
        std::shared_ptr<WebGPUComputeContext> context,
        compute::RenderSceneSnapshot const& snapshot,
        compute::BoundsResult const& bounds,
        io::ManifoldDualContouringOptions const& options,
        WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        if (!context || !context->isValid())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        if (!snapshot.isValid())
        {
            throw std::invalid_argument("WebGPU manifold export requires a valid analytic scene snapshot");
        }
        if (!bounds.isUsable() || !bounds.modelBounds.has_value())
        {
            throw std::invalid_argument("WebGPU manifold export requires authoritative model bounds");
        }
        if (!std::isfinite(options.isoValue) || options.initialDepth == 0U ||
            options.initialDepth > options.maxDepth || options.maxDepth > kMaximumManifoldDepth)
        {
            throw std::invalid_argument("WebGPU manifold export received unsupported depth or iso settings");
        }
        if (!options.enableHierarchicalOctree)
        {
            throw std::invalid_argument(
                "WebGPU manifold export currently requires the shared hierarchical extraction path");
        }
        if (options.minFeatureSize > 0.0F)
        {
            throw std::invalid_argument(
                "WebGPU manifold export does not yet support minimum-feature chunking");
        }
        if (options.enableSharpFeaturePostProcess)
        {
            throw std::invalid_argument(
                "WebGPU manifold export does not yet support sharp-feature post-processing");
        }

        bool const useFastQem = options.simplificationMethod == io::SimplificationMethod::QemFast;
        if (options.simplificationMethod == io::SimplificationMethod::QemSdfAware ||
            (options.simplificationMethod == io::SimplificationMethod::None &&
             options.enableSimplification))
        {
            throw std::invalid_argument(
                "WebGPU manifold export does not yet support SDF-aware QEM simplification");
        }

        throwIfCancelled(shouldCancel);
        auto sdfRequest = makeSdfEvaluationRequest(snapshot, options.isoValue);
        WebGPUManifoldSdfSampler sampler{std::move(context), std::move(sdfRequest)};
        compute::GlobalMortonOctree octree{sampler};
        if (shouldCancel)
        {
            octree.setCancellationCheckCallback(shouldCancel);
        }

        compute::GlobalMortonOctreeConfig config;
        config.initialDepth = options.initialDepth;
        config.maxDepth = options.maxDepth;
        config.isoValue = options.isoValue;
        config.minFeatureSize = options.minFeatureSize;
        config.enableAdaptiveRefinement = false;
        setPaddedBounds(config, bounds, options.maxDepth);
        octree.build(config);
        throwIfCancelled(shouldCancel);
        if (octree.hasSamplingError())
        {
            throw std::runtime_error("WebGPU manifold octree construction failed during SDF sampling");
        }

        io::IndexedTriangleMesh mesh;
        octree.extractMesh(mesh.positions, mesh.normals, mesh.indices);
        throwIfCancelled(shouldCancel);
        if (octree.hasSamplingError())
        {
            throw std::runtime_error("WebGPU manifold mesh extraction failed during SDF sampling");
        }
        if (useFastQem || options.enableSimplification)
        {
            compute::FastQemConfig qemConfig;
            qemConfig.terminationMode = static_cast<compute::SimplificationTerminationMode>(
                static_cast<int>(options.simplificationTerminationMode));
            qemConfig.maxError = options.simplificationMaxError;
            if (options.simplificationTargetTriangles.has_value())
            {
                qemConfig.targetTriangleCount = *options.simplificationTargetTriangles;
            }
            if (options.simplificationTargetReduction.has_value())
            {
                qemConfig.targetReductionPercent = *options.simplificationTargetReduction;
            }
            compute::fastQemSimplify(
                mesh.positions,
                mesh.indices,
                qemConfig,
                [&shouldCancel]() { throwIfCancelled(shouldCancel); });
        }

        if (!mesh.normals.empty() && mesh.normals.size() != mesh.positions.size())
        {
            // QEM changes the vertex set without updating normals. Export writers derive
            // facet normals from the final triangles, so discard the stale optional array.
            mesh.normals.clear();
        }

        bool const hasBoundedFastQem =
            useFastQem &&
            options.simplificationTerminationMode == io::SimplificationTerminationMode::ErrorBounded &&
            std::isfinite(options.simplificationMaxError) && options.simplificationMaxError > 0.0F &&
            options.simplificationMaxError < std::numeric_limits<float>::max();
        // Match the OpenCL exporter: post-QEM projection can move thin-sheet vertices onto a
        // neighboring iso-surface branch, and the QEM threshold is not a strict distance bound.
        if (options.projectToSurface && !hasBoundedFastQem)
        {
            adoptVerticesToSurface(sampler, mesh.positions, shouldCancel);
        }

        std::vector<std::uint32_t> triangleComponents;
        auto const windingRepairStats =
            io::repairTriangleWindingConsistency(mesh.positions, mesh.indices, &triangleComponents);
        auto const orientationSamples = io::makeTriangleOrientationSamples(
            mesh.positions, mesh.indices, triangleComponents, 128U);
        if (!orientationSamples.empty())
        {
            std::vector<Eigen::Vector3f> samplePositions;
            samplePositions.reserve(orientationSamples.size());
            for (auto const& sample : orientationSamples)
            {
                samplePositions.push_back(sample.position);
            }
            std::vector<float> sdfValues;
            std::vector<Eigen::Vector3f> gradients;
            compute::ManifoldSdfSamplingCallbacks callbacks;
            callbacks.shouldCancel = shouldCancel;
            float const maxExtent =
                (Eigen::Vector3f(bounds.modelBounds->max[0], bounds.modelBounds->max[1],
                                 bounds.modelBounds->max[2]) -
                 Eigen::Vector3f(bounds.modelBounds->min[0], bounds.modelBounds->min[1],
                                 bounds.modelBounds->min[2]))
                    .maxCoeff();
            float const cellWidth = maxExtent / std::exp2(static_cast<float>(options.maxDepth));
            float const sdfDistanceTolerance = std::max(1.0e-4F, cellWidth * 0.5F);
            float const gradientEpsilon = std::max(1.0e-5F, cellWidth * 0.01F);
            auto const status = sampler.evaluateSdfGradientBatch(
                samplePositions,
                options.isoValue,
                gradientEpsilon,
                sdfValues,
                gradients,
                callbacks);
            requireCompleted(status, shouldCancel);
            if (sdfValues.size() != orientationSamples.size() ||
                gradients.size() != orientationSamples.size())
            {
                throw std::runtime_error("WebGPU returned incomplete SDF gradients for component orientation");
            }

            std::vector<double> componentVotes(windingRepairStats.components, 0.0);
            for (std::size_t sampleIndex = 0U; sampleIndex < orientationSamples.size(); ++sampleIndex)
            {
                float const sdf = sdfValues[sampleIndex];
                Eigen::Vector3f const& gradient = gradients[sampleIndex];
                float const gradientLength = gradient.norm();
                auto const& sample = orientationSamples[sampleIndex];
                if (!std::isfinite(sdf) || std::abs(sdf) > sdfDistanceTolerance ||
                    !std::isfinite(gradientLength) || gradientLength <= 1.0e-10F ||
                    sample.component >= componentVotes.size())
                {
                    continue;
                }
                componentVotes[sample.component] +=
                    static_cast<double>(sample.normal.dot(gradient / gradientLength));
            }
            (void)io::orientTriangleComponentsByVotes(
                mesh.indices, triangleComponents, componentVotes);
        }

        auto const validationError = mesh.validationError(kWebGpuOutputValidationPolicy);
        if (validationError.has_value())
        {
            throw std::runtime_error("WebGPU manifold extraction produced an invalid mesh: " +
                                     *validationError);
        }
        auto const topologyError = mesh.closedOrientedTopologyError(kWebGpuOutputValidationPolicy);
        if (topologyError.has_value())
        {
            throw std::runtime_error("WebGPU manifold extraction produced invalid topology: " +
                                     *topologyError);
        }
        return mesh;
    }

    void WebGPUManifoldDualContouringExporter::exportStl(
        std::filesystem::path const& filePath,
        std::shared_ptr<WebGPUComputeContext> context,
        compute::RenderSceneSnapshot const& snapshot,
        compute::BoundsResult const& bounds,
        io::ManifoldDualContouringOptions const& options,
        WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        auto mesh = generateMesh(std::move(context), snapshot, bounds, options, std::move(shouldCancel));
        io::writeIndexedMeshToBinaryStl(filePath, mesh, kWebGpuOutputValidationPolicy);
    }

    void WebGPUManifoldDualContouringExporter::export3mf(
        std::filesystem::path const& filePath,
        std::shared_ptr<WebGPUComputeContext> context,
        compute::RenderSceneSnapshot const& snapshot,
        compute::BoundsResult const& bounds,
        io::ManifoldDualContouringOptions const& options,
        WebGPUSdfEvaluator::CancellationPredicate shouldCancel)
    {
        auto mesh = generateMesh(std::move(context), snapshot, bounds, options, std::move(shouldCancel));
        io::IndexedTriangleMesh3mfWriter writer;
        writer.exportMesh(filePath,
                          mesh,
                          filePath.stem().string(),
                          kWebGpuOutputValidationPolicy);
    }
} // namespace gladius::webgpu
