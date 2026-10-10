#pragma once

#include "ManifoldSdfSampler.h"

#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::compute
{
    class ManifoldDualContouringProgram;

    /// @brief OpenCL adapter retaining the legacy manifold sampler behavior.
    class OpenCLManifoldSdfSampler final : public ManifoldSdfSampler
    {
      public:
        OpenCLManifoldSdfSampler(ComputeCore& core, ManifoldDualContouringProgram& program);

        [[nodiscard]] ManifoldSdfSamplingStatus sampleSdf(
            Eigen::Vector3f const& position,
            Eigen::Vector3f const& domainMin,
            Eigen::Vector3f const& domainSize,
            float& outValue,
            ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] ManifoldSdfSamplingStatus evaluateSdfBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            std::vector<float>& outValues,
            ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] ManifoldSdfSamplingStatus evaluateSdfGradientBatch(
            std::vector<Eigen::Vector3f> const& positions,
            float isoValue,
            float epsilon,
            std::vector<float>& outValues,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] ManifoldSdfSamplingStatus refineHermiteEdgesBatch(
            std::vector<Eigen::Vector3f> const& starts,
            std::vector<Eigen::Vector3f> const& ends,
            std::vector<float> const& startValues,
            std::vector<float> const& endValues,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const override;

        [[nodiscard]] ManifoldSdfSamplingStatus projectToSurfaceBatch(
            std::vector<Eigen::Vector3f> const& positions,
            std::vector<Eigen::Vector3f> const& boundsMin,
            std::vector<Eigen::Vector3f> const& boundsMax,
            float isoValue,
            float epsilon,
            std::vector<Eigen::Vector3f>& outPositions,
            std::vector<Eigen::Vector3f>& outGradients,
            ManifoldSdfSamplingCallbacks const& callbacks) const override;

      private:
        ComputeCore& m_core;
        ManifoldDualContouringProgram& m_program;
        mutable bool m_precomputedGridRead{false};
    };
} // namespace gladius::compute
