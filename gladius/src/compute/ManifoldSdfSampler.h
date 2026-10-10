#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gladius::compute
{
    /// @brief Outcome of one backend-neutral manifold SDF sampling operation.
    enum class ManifoldSdfSamplingStatus : std::uint8_t
    {
        Completed,
        Cancelled,
        Unavailable
    };

    /// @brief Optional cancellation and progress hooks for bounded sampling work.
    struct ManifoldSdfSamplingCallbacks
    {
        std::function<bool()> shouldCancel;
        std::function<void(float)> reportProgress;
    };

    /// @brief Backend-neutral SDF queries required by the manifold octree mesher.
    ///
    /// Batch implementations should bound dispatch/readback sizes, poll shouldCancel between
    /// batches, report progress in [0, 1], and only expose complete output arrays on success.
    class ManifoldSdfSampler
    {
      public:
        virtual ~ManifoldSdfSampler() = default;

        /// @brief Samples the backend's grid or scalar fallback at one world position.
        virtual ManifoldSdfSamplingStatus sampleSdf(Eigen::Vector3f const& position,
                                                     Eigen::Vector3f const& domainMin,
                                                     Eigen::Vector3f const& domainSize,
                                                     float& outValue,
                                                     ManifoldSdfSamplingCallbacks const& callbacks) const = 0;

        /// @brief Evaluates signed distances at a batch of world positions.
        virtual ManifoldSdfSamplingStatus evaluateSdfBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            std::vector<float>& outValues,
            ManifoldSdfSamplingCallbacks const& callbacks) const = 0;

        /// @brief Evaluates signed distances and gradients for a batch of positions.
        virtual ManifoldSdfSamplingStatus evaluateSdfGradientBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            float epsilon,
            std::vector<float>& outValues,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const = 0;

        /// @brief Refines edge zero-crossings and computes gradients in one batch.
        virtual ManifoldSdfSamplingStatus refineHermiteEdgesBatch(
            std::vector<Eigen::Vector3f> const& starts,
            std::vector<Eigen::Vector3f> const& ends,
            std::vector<float> const& startValues,
            std::vector<float> const& endValues,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const = 0;

        /// @brief Projects a batch of vertices to the iso-surface within per-vertex bounds.
        virtual ManifoldSdfSamplingStatus projectToSurfaceBatch(
            std::vector<Eigen::Vector3f> const& positions,
            std::vector<Eigen::Vector3f> const& boundsMin,
            std::vector<Eigen::Vector3f> const& boundsMax,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const = 0;
    };
} // namespace gladius::compute
