#pragma once

#include "compute/ManifoldSdfSampler.h"
#include "compute/SdfEvaluation.h"
#include "webgpu/WebGPUSdfEvaluator.h"

#include <memory>
#include <vector>

namespace gladius::webgpu
{
    /// @brief WebGPU-backed implementation of the shared manifold mesher's SDF queries.
    class WebGPUManifoldSdfSampler final : public compute::ManifoldSdfSampler
    {
      public:
        WebGPUManifoldSdfSampler(
            std::shared_ptr<WebGPUComputeContext> context,
            compute::SdfEvaluationRequest request);

        [[nodiscard]] compute::ManifoldSdfSamplingStatus sampleSdf(
            Eigen::Vector3f const& position,
            Eigen::Vector3f const& domainMin,
            Eigen::Vector3f const& domainSize,
            float& outValue,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus evaluateSdfBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            std::vector<float>& outValues,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus evaluateSdfGradientBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            float epsilon,
            std::vector<float>& outValues,
            std::vector<Eigen::Vector3f>& outGradients,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus refineHermiteEdgesBatch(
            std::vector<Eigen::Vector3f> const& starts,
            std::vector<Eigen::Vector3f> const& ends,
            std::vector<float> const& startValues,
            std::vector<float> const& endValues,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus projectToSurfaceBatch(
            std::vector<Eigen::Vector3f> const& positions,
            std::vector<Eigen::Vector3f> const& boundsMin,
            std::vector<Eigen::Vector3f> const& boundsMax,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const override;

      private:
                [[nodiscard]] compute::ManifoldSdfSamplingStatus evaluateSdfGradientBatchInternal(
                        std::vector<Eigen::Vector3f> const& positions,
                        float isoValue,
                        float epsilon,
                        std::vector<float>& outValues,
                        std::vector<Eigen::Vector3f>& outGradients,
                        compute::ManifoldSdfSamplingCallbacks const& callbacks) const;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus evaluateRawSdfBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            std::vector<float>& outValues,
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const;

        [[nodiscard]] compute::ManifoldSdfSamplingStatus reportQueryStart(
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const;
        [[nodiscard]] compute::ManifoldSdfSamplingStatus reportQueryProgress(
            compute::ManifoldSdfSamplingCallbacks const& callbacks,
            float progress) const;
        [[nodiscard]] compute::ManifoldSdfSamplingStatus reportQueryComplete(
            compute::ManifoldSdfSamplingCallbacks const& callbacks) const;

        mutable WebGPUSdfEvaluator m_evaluator;
        compute::SdfEvaluationRequest m_request;
    };
} // namespace gladius::webgpu
