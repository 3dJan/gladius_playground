#include "WebGPUManifoldSdfSampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        constexpr float kHermiteTolerance = 1.0e-5F;
        constexpr std::size_t kHermiteBisectionIterations = 10U;
        constexpr std::size_t kNewtonProjectionIterations = 16U;

        [[nodiscard]] bool isCancelled(
            compute::ManifoldSdfSamplingCallbacks const& callbacks)
        {
            return callbacks.shouldCancel && callbacks.shouldCancel();
        }

        void reportProgress(
            compute::ManifoldSdfSamplingCallbacks const& callbacks,
            float const progress)
        {
            if (callbacks.reportProgress)
            {
                callbacks.reportProgress(std::clamp(progress, 0.0F, 1.0F));
            }
        }
    } // namespace

    WebGPUManifoldSdfSampler::WebGPUManifoldSdfSampler(
        std::shared_ptr<WebGPUComputeContext> context,
        compute::SdfEvaluationRequest request)
        : m_evaluator(std::move(context))
        , m_request(std::move(request))
    {
        if (!m_request.positions.empty())
        {
            throw std::invalid_argument(
                "WebGPU manifold sampler requires a generated-position SDF request");
        }
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::reportQueryStart(
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        if (isCancelled(callbacks))
        {
            return compute::ManifoldSdfSamplingStatus::Cancelled;
        }
        reportProgress(callbacks, 0.0F);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::reportQueryProgress(
        compute::ManifoldSdfSamplingCallbacks const& callbacks,
        float const progress) const
    {
        if (isCancelled(callbacks))
        {
            return compute::ManifoldSdfSamplingStatus::Cancelled;
        }
        reportProgress(callbacks, progress);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::reportQueryComplete(
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        if (isCancelled(callbacks))
        {
            return compute::ManifoldSdfSamplingStatus::Cancelled;
        }
        reportProgress(callbacks, 1.0F);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::evaluateRawSdfBatch(
        std::vector<Eigen::Vector3f> const& positions,
        float const isoValue,
        std::vector<float>& outValues,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        if (isCancelled(callbacks))
        {
            return compute::ManifoldSdfSamplingStatus::Cancelled;
        }
        if (positions.empty())
        {
            return compute::ManifoldSdfSamplingStatus::Completed;
        }
        if (!std::isfinite(isoValue) ||
            positions.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }

        std::vector<float> values(positions.size(), std::numeric_limits<float>::quiet_NaN());
        std::size_t writtenCount = 0U;
        auto const evaluateRequest = [&](compute::SdfEvaluationRequest const& request)
        {
            return m_evaluator.evaluateGenerated(
                request,
                positions.size(),
                [&positions](std::size_t const offset,
                             std::size_t const count,
                             std::vector<std::array<float, 3>>& batchPositions)
                {
                    batchPositions.reserve(count);
                    for (std::size_t index = offset; index < offset + count; ++index)
                    {
                        auto const& position = positions[index];
                        batchPositions.push_back({position.x(), position.y(), position.z()});
                    }
                },
                [&values, &writtenCount](std::size_t const offset,
                                         std::vector<float>&& batchValues)
                {
                    if (offset > values.size() || batchValues.size() > values.size() - offset)
                    {
                        return;
                    }
                    std::copy(batchValues.begin(),
                              batchValues.end(),
                              values.begin() + static_cast<std::ptrdiff_t>(offset));
                    writtenCount += batchValues.size();
                },
                callbacks.shouldCancel);
        };

        try
        {
            bool completed = false;
            if (isoValue == m_request.isoValue)
            {
                completed = evaluateRequest(m_request);
            }
            else
            {
                auto request = m_request;
                request.isoValue = isoValue;
                completed = evaluateRequest(request);
            }
            if (!completed)
            {
                return isCancelled(callbacks) ? compute::ManifoldSdfSamplingStatus::Cancelled
                                              : compute::ManifoldSdfSamplingStatus::Unavailable;
            }
        }
        catch (std::exception const&)
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }

        if (isCancelled(callbacks))
        {
            return compute::ManifoldSdfSamplingStatus::Cancelled;
        }
        if (writtenCount != positions.size())
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }
        outValues = std::move(values);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::sampleSdf(
        Eigen::Vector3f const& position,
        Eigen::Vector3f const& domainMin,
        Eigen::Vector3f const& domainSize,
        float& outValue,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        (void) domainMin;
        (void) domainSize;
        outValue = 0.0F;
        if (auto const status = reportQueryStart(callbacks);
            status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }

        std::vector<float> values;
        auto const status = evaluateRawSdfBatch(
            {position}, m_request.isoValue, values, callbacks);
        if (status != compute::ManifoldSdfSamplingStatus::Completed || values.size() != 1U)
        {
            return status == compute::ManifoldSdfSamplingStatus::Completed
                     ? compute::ManifoldSdfSamplingStatus::Unavailable
                     : status;
        }
        if (auto const completeStatus = reportQueryComplete(callbacks);
            completeStatus != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return completeStatus;
        }
        outValue = values.front();
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::evaluateSdfBatch(
        std::vector<Eigen::Vector3f> const& positions,
        float const isoValue,
        std::vector<float>& outValues,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        if (auto const status = reportQueryStart(callbacks);
            status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }

        auto const status = evaluateRawSdfBatch(positions, isoValue, outValues, callbacks);
        if (status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            outValues.clear();
            return status;
        }
        if (outValues.size() != positions.size())
        {
            outValues.clear();
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const completeStatus = reportQueryComplete(callbacks);
            completeStatus != compute::ManifoldSdfSamplingStatus::Completed)
        {
            outValues.clear();
            return completeStatus;
        }
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::evaluateSdfGradientBatch(
        std::vector<Eigen::Vector3f> const& positions,
        float const isoValue,
        float const epsilon,
        std::vector<float>& outValues,
        std::vector<Eigen::Vector3f>& outGradients,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        outGradients.clear();
        if (auto const status = reportQueryStart(callbacks);
            status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }

        std::vector<float> values;
        std::vector<Eigen::Vector3f> gradients;
        auto const status = evaluateSdfGradientBatchInternal(
            positions, isoValue, epsilon, values, gradients, callbacks);
        if (status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (auto const completeStatus = reportQueryComplete(callbacks);
            completeStatus != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return completeStatus;
        }
        outValues = std::move(values);
        outGradients = std::move(gradients);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::evaluateSdfGradientBatchInternal(
        std::vector<Eigen::Vector3f> const& positions,
        float const isoValue,
        float const epsilon,
        std::vector<float>& outValues,
        std::vector<Eigen::Vector3f>& outGradients,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outValues.clear();
        outGradients.clear();
        if (!std::isfinite(epsilon) || epsilon <= 0.0F ||
            positions.size() > std::numeric_limits<std::size_t>::max() / 7U)
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }
        if (positions.empty())
        {
            return compute::ManifoldSdfSamplingStatus::Completed;
        }

        std::vector<Eigen::Vector3f> samplePositions(positions.size() * 7U);
        for (std::size_t index = 0U; index < positions.size(); ++index)
        {
            auto const base = index * 7U;
            auto const& position = positions[index];
            samplePositions[base] = position;
            samplePositions[base + 1U] = position + Eigen::Vector3f(epsilon, 0.0F, 0.0F);
            samplePositions[base + 2U] = position - Eigen::Vector3f(epsilon, 0.0F, 0.0F);
            samplePositions[base + 3U] = position + Eigen::Vector3f(0.0F, epsilon, 0.0F);
            samplePositions[base + 4U] = position - Eigen::Vector3f(0.0F, epsilon, 0.0F);
            samplePositions[base + 5U] = position + Eigen::Vector3f(0.0F, 0.0F, epsilon);
            samplePositions[base + 6U] = position - Eigen::Vector3f(0.0F, 0.0F, epsilon);
        }

        std::vector<float> samples;
        auto const status = evaluateRawSdfBatch(samplePositions, isoValue, samples, callbacks);
        if (status != compute::ManifoldSdfSamplingStatus::Completed ||
            samples.size() != samplePositions.size())
        {
            return status == compute::ManifoldSdfSamplingStatus::Completed
                     ? compute::ManifoldSdfSamplingStatus::Unavailable
                     : status;
        }

        std::vector<float> values(positions.size());
        std::vector<Eigen::Vector3f> gradients(positions.size());
        float const inverseTwoEpsilon = 1.0F / (2.0F * epsilon);
        for (std::size_t index = 0U; index < positions.size(); ++index)
        {
            auto const base = index * 7U;
            values[index] = samples[base];
            gradients[index] = Eigen::Vector3f(
                (samples[base + 1U] - samples[base + 2U]) * inverseTwoEpsilon,
                (samples[base + 3U] - samples[base + 4U]) * inverseTwoEpsilon,
                (samples[base + 5U] - samples[base + 6U]) * inverseTwoEpsilon);
        }
        outValues = std::move(values);
        outGradients = std::move(gradients);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::refineHermiteEdgesBatch(
        std::vector<Eigen::Vector3f> const& starts,
        std::vector<Eigen::Vector3f> const& ends,
        std::vector<float> const& startValues,
        std::vector<float> const& endValues,
        float const isoValue,
        float const epsilon,
        std::vector<Eigen::Vector3f>& outPositions,
        std::vector<Eigen::Vector3f>& outGradients,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outPositions.clear();
        outGradients.clear();
        if (starts.size() != ends.size() || starts.size() != startValues.size() ||
            starts.size() != endValues.size() || !std::isfinite(epsilon) || epsilon <= 0.0F)
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = reportQueryStart(callbacks);
            status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (starts.empty())
        {
            return reportQueryComplete(callbacks);
        }

        std::vector<Eigen::Vector3f> lows = starts;
        std::vector<Eigen::Vector3f> highs = ends;
        std::vector<float> lowValues = startValues;
        std::vector<float> highValues = endValues;
        std::vector<Eigen::Vector3f> positions(starts.size());
        std::vector<Eigen::Vector3f> gradients(starts.size(), Eigen::Vector3f::Zero());
        std::vector<std::uint8_t> needsGradient(starts.size(), 1U);
        std::vector<std::size_t> activeIndices;
        activeIndices.reserve(starts.size());

        for (std::size_t index = 0U; index < starts.size(); ++index)
        {
            if (std::abs(lowValues[index]) <= kHermiteTolerance)
            {
                positions[index] = lows[index];
                needsGradient[index] = 0U;
            }
            else if (std::abs(highValues[index]) <= kHermiteTolerance)
            {
                positions[index] = highs[index];
                needsGradient[index] = 0U;
            }
            else
            {
                activeIndices.push_back(index);
            }
        }

        for (std::size_t iteration = 0U; iteration < kHermiteBisectionIterations &&
                                             !activeIndices.empty();
             ++iteration)
        {
            std::vector<Eigen::Vector3f> midpoints;
            midpoints.reserve(activeIndices.size());
            for (std::size_t index : activeIndices)
            {
                midpoints.push_back((lows[index] + highs[index]) * 0.5F);
            }

            std::vector<float> midValues;
            auto const status = evaluateRawSdfBatch(midpoints, isoValue, midValues, callbacks);
            if (status != compute::ManifoldSdfSamplingStatus::Completed ||
                midValues.size() != activeIndices.size())
            {
                return status == compute::ManifoldSdfSamplingStatus::Completed
                         ? compute::ManifoldSdfSamplingStatus::Unavailable
                         : status;
            }

            std::vector<std::size_t> nextActiveIndices;
            nextActiveIndices.reserve(activeIndices.size());
            for (std::size_t activeIndex = 0U; activeIndex < activeIndices.size(); ++activeIndex)
            {
                std::size_t const edgeIndex = activeIndices[activeIndex];
                Eigen::Vector3f const midpoint = midpoints[activeIndex];
                float const midValue = midValues[activeIndex];
                if (std::abs(midValue) < kHermiteTolerance)
                {
                    lows[edgeIndex] = midpoint;
                    highs[edgeIndex] = midpoint;
                    needsGradient[edgeIndex] = 1U;
                    continue;
                }

                if (midValue * lowValues[edgeIndex] < 0.0F)
                {
                    highs[edgeIndex] = midpoint;
                    highValues[edgeIndex] = midValue;
                }
                else
                {
                    lows[edgeIndex] = midpoint;
                    lowValues[edgeIndex] = midValue;
                }
                nextActiveIndices.push_back(edgeIndex);
            }
            activeIndices = std::move(nextActiveIndices);
            if (auto const progressStatus = reportQueryProgress(
                    callbacks,
                    static_cast<float>(iteration + 1U) /
                      static_cast<float>(kHermiteBisectionIterations + 1U));
                progressStatus != compute::ManifoldSdfSamplingStatus::Completed)
            {
                return progressStatus;
            }
        }

        std::vector<std::size_t> gradientIndices;
        std::vector<Eigen::Vector3f> gradientPositions;
        gradientIndices.reserve(starts.size());
        gradientPositions.reserve(starts.size());
        for (std::size_t index = 0U; index < starts.size(); ++index)
        {
            if (needsGradient[index] != 0U)
            {
                positions[index] = (lows[index] + highs[index]) * 0.5F;
                gradientIndices.push_back(index);
                gradientPositions.push_back(positions[index]);
            }
        }

        if (!gradientPositions.empty())
        {
            std::vector<float> gradientValues;
            std::vector<Eigen::Vector3f> sampledGradients;
            auto const status = evaluateSdfGradientBatchInternal(
                gradientPositions, isoValue, epsilon, gradientValues, sampledGradients, callbacks);
            if (status != compute::ManifoldSdfSamplingStatus::Completed ||
                sampledGradients.size() != gradientIndices.size())
            {
                return status == compute::ManifoldSdfSamplingStatus::Completed
                         ? compute::ManifoldSdfSamplingStatus::Unavailable
                         : status;
            }
            for (std::size_t i = 0U; i < gradientIndices.size(); ++i)
            {
                gradients[gradientIndices[i]] = sampledGradients[i];
            }
        }

        if (auto const completeStatus = reportQueryComplete(callbacks);
            completeStatus != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return completeStatus;
        }
        outPositions = std::move(positions);
        outGradients = std::move(gradients);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }

    compute::ManifoldSdfSamplingStatus WebGPUManifoldSdfSampler::projectToSurfaceBatch(
        std::vector<Eigen::Vector3f> const& positions,
        std::vector<Eigen::Vector3f> const& boundsMin,
        std::vector<Eigen::Vector3f> const& boundsMax,
        float const isoValue,
        float const epsilon,
        std::vector<Eigen::Vector3f>& outPositions,
        std::vector<Eigen::Vector3f>& outGradients,
        compute::ManifoldSdfSamplingCallbacks const& callbacks) const
    {
        outPositions.clear();
        outGradients.clear();
        if (positions.size() != boundsMin.size() || positions.size() != boundsMax.size() ||
            !std::isfinite(epsilon) || epsilon <= 0.0F)
        {
            return compute::ManifoldSdfSamplingStatus::Unavailable;
        }
        if (auto const status = reportQueryStart(callbacks);
            status != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return status;
        }
        if (positions.empty())
        {
            return reportQueryComplete(callbacks);
        }

        std::vector<Eigen::Vector3f> projected = positions;
        std::vector<std::uint8_t> active(positions.size(), 1U);
        for (std::size_t iteration = 0U; iteration < kNewtonProjectionIterations; ++iteration)
        {
            std::vector<std::size_t> activeIndices;
            std::vector<Eigen::Vector3f> activePositions;
            activeIndices.reserve(positions.size());
            activePositions.reserve(positions.size());
            for (std::size_t index = 0U; index < active.size(); ++index)
            {
                if (active[index] != 0U)
                {
                    activeIndices.push_back(index);
                    activePositions.push_back(projected[index]);
                }
            }
            if (activeIndices.empty())
            {
                break;
            }

            std::vector<float> values;
            std::vector<Eigen::Vector3f> gradients;
            auto const status = evaluateSdfGradientBatchInternal(
                activePositions, isoValue, epsilon, values, gradients, callbacks);
            if (status != compute::ManifoldSdfSamplingStatus::Completed ||
                values.size() != activeIndices.size() || gradients.size() != activeIndices.size())
            {
                return status == compute::ManifoldSdfSamplingStatus::Completed
                         ? compute::ManifoldSdfSamplingStatus::Unavailable
                         : status;
            }

            for (std::size_t activeIndex = 0U; activeIndex < activeIndices.size(); ++activeIndex)
            {
                std::size_t const index = activeIndices[activeIndex];
                if (std::abs(values[activeIndex]) < kHermiteTolerance)
                {
                    active[index] = 0U;
                    continue;
                }

                Eigen::Vector3f const& gradient = gradients[activeIndex];
                float const gradientSquaredNorm = gradient.squaredNorm();
                if (gradientSquaredNorm < 1.0e-20F)
                {
                    active[index] = 0U;
                    continue;
                }

                float const step = values[activeIndex] / gradientSquaredNorm;
                projected[index] -= step * gradient;
                projected[index] = projected[index].cwiseMax(boundsMin[index]).cwiseMin(boundsMax[index]);
            }

            if (auto const progressStatus = reportQueryProgress(
                    callbacks,
                    static_cast<float>(iteration + 1U) /
                      static_cast<float>(kNewtonProjectionIterations + 1U));
                progressStatus != compute::ManifoldSdfSamplingStatus::Completed)
            {
                return progressStatus;
            }
        }

        std::vector<float> finalValues;
        std::vector<Eigen::Vector3f> finalGradients;
        auto const finalStatus = evaluateSdfGradientBatchInternal(
            projected, isoValue, epsilon, finalValues, finalGradients, callbacks);
        if (finalStatus != compute::ManifoldSdfSamplingStatus::Completed ||
            finalGradients.size() != projected.size())
        {
            return finalStatus == compute::ManifoldSdfSamplingStatus::Completed
                     ? compute::ManifoldSdfSamplingStatus::Unavailable
                     : finalStatus;
        }
        if (auto const completeStatus = reportQueryComplete(callbacks);
            completeStatus != compute::ManifoldSdfSamplingStatus::Completed)
        {
            return completeStatus;
        }
        outPositions = std::move(projected);
        outGradients = std::move(finalGradients);
        return compute::ManifoldSdfSamplingStatus::Completed;
    }
} // namespace gladius::webgpu
