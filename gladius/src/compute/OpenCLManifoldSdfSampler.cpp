#include "OpenCLManifoldSdfSampler.h"

#include "ComputeCore.h"
#include "ManifoldDualContouringProgram.h"
#include "../ResourceContext.h"
#include "SdfGridInterpolator.h"

#include <algorithm>
#include <utility>

namespace gladius::compute
{
    namespace
    {
        [[nodiscard]] bool isCancelled(ManifoldSdfSamplingCallbacks const& callbacks)
        {
            return callbacks.shouldCancel && callbacks.shouldCancel();
        }

        void reportProgress(ManifoldSdfSamplingCallbacks const& callbacks, float progress)
        {
            if (callbacks.reportProgress)
            {
                callbacks.reportProgress(std::clamp(progress, 0.0F, 1.0F));
            }
        }

        [[nodiscard]] ManifoldSdfSamplingStatus beginQuery(
            ManifoldSdfSamplingCallbacks const& callbacks)
        {
            if (isCancelled(callbacks))
            {
                return ManifoldSdfSamplingStatus::Cancelled;
            }
            reportProgress(callbacks, 0.0F);
            return ManifoldSdfSamplingStatus::Completed;
        }

        [[nodiscard]] ManifoldSdfSamplingStatus completeQuery(
            ManifoldSdfSamplingCallbacks const& callbacks)
        {
            if (isCancelled(callbacks))
            {
                return ManifoldSdfSamplingStatus::Cancelled;
            }
            reportProgress(callbacks, 1.0F);
            return ManifoldSdfSamplingStatus::Completed;
        }
    } // namespace

    OpenCLManifoldSdfSampler::OpenCLManifoldSdfSampler(
        ComputeCore& core,
        ManifoldDualContouringProgram& program)
        : m_core(core)
        , m_program(program)
    {
    }

    ManifoldSdfSamplingStatus OpenCLManifoldSdfSampler::sampleSdf(
        Eigen::Vector3f const& position,
        Eigen::Vector3f const& domainMin,
        Eigen::Vector3f const& domainSize,
        float& outValue,
        ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValue = 0.0F;
        if (auto const status = beginQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }

        auto const resources = m_core.getResourceContext();
        if (!resources)
        {
            return ManifoldSdfSamplingStatus::Unavailable;
        }

        auto& sdfBuffer = resources->getPrecompSdfBuffer();
        if (!m_precomputedGridRead)
        {
            sdfBuffer.read();
            m_precomputedGridRead = true;
        }
        auto const& data = sdfBuffer.getData();
        outValue = sampleTrilinearSdfGrid(
            position,
            domainMin,
            domainSize,
            sdfBuffer.getWidth(),
            sdfBuffer.getHeight(),
            sdfBuffer.getDepth(),
            [&data](std::size_t index)
            {
                return index < data.size() ? data[index].s[3] : 0.0F;
            });
        return completeQuery(callbacks);
    }

    ManifoldSdfSamplingStatus OpenCLManifoldSdfSampler::evaluateSdfBatch(
        std::vector<Eigen::Vector3f> const& positions,
        float isoValue,
        std::vector<float>& outValues,
        ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        if (auto const status = beginQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (positions.empty())
        {
            outValues.clear();
            return completeQuery(callbacks);
        }

        auto const primitives = m_core.getPrimitives();
        if (!primitives)
        {
            outValues.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }

        auto values = m_program.evaluateSdfBatch(positions, *primitives, isoValue);
        if (values.size() != positions.size())
        {
            outValues.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = completeQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            outValues.clear();
            return status;
        }
        outValues = std::move(values);
        return ManifoldSdfSamplingStatus::Completed;
    }

    ManifoldSdfSamplingStatus OpenCLManifoldSdfSampler::evaluateSdfGradientBatch(
        std::vector<Eigen::Vector3f> const& positions,
        float isoValue,
        float epsilon,
        std::vector<float>& outValues,
        std::vector<Eigen::Vector3f>& outGradients,
        ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        outGradients.clear();
        if (auto const status = beginQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }

        if (positions.empty())
        {
            return completeQuery(callbacks);
        }

        auto const primitives = m_core.getPrimitives();
        if (!primitives)
        {
            return ManifoldSdfSamplingStatus::Unavailable;
        }

        std::vector<float> values;
        std::vector<Eigen::Vector3f> gradients;
        m_program.evaluateSdfGradientBatch(positions, *primitives, isoValue, epsilon, values, gradients);
        if (values.size() != positions.size() || gradients.size() != positions.size())
        {
            outValues.clear();
            outGradients.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = completeQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            outValues.clear();
            outGradients.clear();
            return status;
        }
        outValues = std::move(values);
        outGradients = std::move(gradients);
        return ManifoldSdfSamplingStatus::Completed;
    }

    ManifoldSdfSamplingStatus OpenCLManifoldSdfSampler::refineHermiteEdgesBatch(
        std::vector<Eigen::Vector3f> const& starts,
        std::vector<Eigen::Vector3f> const& ends,
        std::vector<float> const& startValues,
        std::vector<float> const& endValues,
        float isoValue,
        float epsilon,
        std::vector<Eigen::Vector3f>& outPositions,
        std::vector<Eigen::Vector3f>& outGradients,
        ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outPositions.clear();
        outGradients.clear();
        if (auto const status = beginQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (starts.size() != ends.size() || starts.size() != startValues.size() ||
            starts.size() != endValues.size())
        {
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (starts.empty())
        {
            outPositions.clear();
            outGradients.clear();
            return completeQuery(callbacks);
        }

        auto const primitives = m_core.getPrimitives();
        if (!primitives)
        {
            outPositions.clear();
            outGradients.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }

        std::vector<Eigen::Vector3f> positions;
        std::vector<Eigen::Vector3f> gradients;
        m_program.hermiteBisectAndGradientBatch(
            starts, ends, startValues, endValues, *primitives, isoValue, epsilon, positions, gradients);
        if (positions.size() != starts.size() || gradients.size() != starts.size())
        {
            outPositions.clear();
            outGradients.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = completeQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            outPositions.clear();
            outGradients.clear();
            return status;
        }
        outPositions = std::move(positions);
        outGradients = std::move(gradients);
        return ManifoldSdfSamplingStatus::Completed;
    }

    ManifoldSdfSamplingStatus OpenCLManifoldSdfSampler::projectToSurfaceBatch(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<Eigen::Vector3f> const& boundsMin,
        std::vector<Eigen::Vector3f> const& boundsMax,
        float isoValue,
        float epsilon,
        std::vector<Eigen::Vector3f>& outPositions,
        std::vector<Eigen::Vector3f>& outGradients,
        ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outPositions.clear();
        outGradients.clear();
        if (auto const status = beginQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (positions.size() != boundsMin.size() || positions.size() != boundsMax.size())
        {
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (positions.empty())
        {
            outPositions.clear();
            outGradients.clear();
            return completeQuery(callbacks);
        }

        auto const primitives = m_core.getPrimitives();
        if (!primitives)
        {
            outPositions.clear();
            outGradients.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }

        std::vector<Eigen::Vector3f> projectedPositions;
        std::vector<Eigen::Vector3f> gradients;
        m_program.newtonProjectToSurfaceBatch(
            positions, boundsMin, boundsMax, *primitives, isoValue, epsilon, projectedPositions, gradients);
        if (projectedPositions.size() != positions.size() || gradients.size() != positions.size())
        {
            outPositions.clear();
            outGradients.clear();
            return ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = completeQuery(callbacks); status != ManifoldSdfSamplingStatus::Completed)
        {
            outPositions.clear();
            outGradients.clear();
            return status;
        }
        outPositions = std::move(projectedPositions);
        outGradients = std::move(gradients);
        return ManifoldSdfSamplingStatus::Completed;
    }
} // namespace gladius::compute
