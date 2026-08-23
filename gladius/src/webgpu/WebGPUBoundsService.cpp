#include "webgpu/WebGPUBoundsService.h"

#include "webgpu/WebGPUSdfEvaluator.h"
#include "webgpu/WebGPUSdfShaderComposer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gladius::webgpu
{
    struct WebGPUBoundsService::State
    {
        mutable std::mutex mutex;
        std::shared_ptr<const compute::RenderSceneSnapshot> snapshot;
        std::optional<compute::BoundsResult> cachedResult;
    };

    namespace
    {
        constexpr std::size_t MAX_GRID_SAMPLES = 64u * 1024u * 1024u;

        struct GridDimensions
        {
            std::array<std::uint32_t, 3> points{};
            std::size_t count{};
        };

        struct BoundsAccumulator
        {
            compute::RenderBounds bounds{};
            bool hasValue{};

            void add(std::array<float, 3> const & point) noexcept
            {
                if (!hasValue)
                {
                    bounds.min = point;
                    bounds.max = point;
                    hasValue = true;
                    return;
                }

                for (std::size_t axis = 0u; axis < 3u; ++axis)
                {
                    bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
                    bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
                }
            }

            void add(compute::RenderBounds const & other) noexcept
            {
                add(other.min);
                add(other.max);
            }
        };

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

        [[nodiscard]] std::optional<GridDimensions>
        calculateGridDimensions(compute::BoundsRequest const & request)
        {
            GridDimensions dimensions;
            std::size_t count = 1u;
            for (std::size_t axis = 0u; axis < 3u; ++axis)
            {
                if (request.probeSettings.resolution[axis] ==
                    std::numeric_limits<std::uint32_t>::max())
                {
                    return std::nullopt;
                }

                dimensions.points[axis] = request.probeSettings.resolution[axis] + 1u;
                if (!checkedProduct(count,
                                   static_cast<std::size_t>(dimensions.points[axis]),
                                   count) ||
                    count > MAX_GRID_SAMPLES)
                {
                    return std::nullopt;
                }
            }

            dimensions.count = count;
            return dimensions;
        }

        [[nodiscard]] std::size_t gridIndex(GridDimensions const & dimensions,
                                             std::size_t const x,
                                             std::size_t const y,
                                             std::size_t const z) noexcept
        {
            return (z * static_cast<std::size_t>(dimensions.points[1]) + y) *
                       static_cast<std::size_t>(dimensions.points[0]) +
                   x;
        }

        [[nodiscard]] std::array<float, 3>
        gridPoint(compute::RenderEvaluationDomain const & domain,
                  compute::BoundsRequest const & request,
                  std::size_t const x,
                  std::size_t const y,
                  std::size_t const z) noexcept
        {
            auto const coordinate = [](float const minimum,
                                       float const maximum,
                                       std::size_t const index,
                                       std::uint32_t const resolution)
            {
                return minimum + (maximum - minimum) *
                                   (static_cast<float>(index) / static_cast<float>(resolution));
            };

            return {coordinate(domain.min[0], domain.max[0], x, request.probeSettings.resolution[0]),
                    coordinate(domain.min[1], domain.max[1], y, request.probeSettings.resolution[1]),
                    coordinate(domain.min[2], domain.max[2], z, request.probeSettings.resolution[2])};
        }

        [[nodiscard]] compute::BoundsResult makeResultBase(compute::BoundsRequest const & request)
        {
            compute::BoundsResult result;
            result.probeDomain = request.probeDomain;
            result.freshness = request.freshness;
            return result;
        }

        [[nodiscard]] compute::BoundsResult makeError(compute::BoundsRequest const & request,
                                                      compute::BoundsResultStatus const status,
                                                      compute::BoundsErrorCode const errorCode,
                                                      std::string message)
        {
            auto result = makeResultBase(request);
            result.status = status;
            result.errorCode = errorCode;
            result.diagnostics.message = std::move(message);
            return result;
        }

        void cancelResult(compute::BoundsResult & result)
        {
            result.status = compute::BoundsResultStatus::Cancelled;
            result.source = compute::BoundsSource::None;
            result.guarantee = compute::BoundsGuarantee::None;
            result.errorCode = compute::BoundsErrorCode::Cancelled;
            result.modelBounds.reset();
            result.authoritative = false;
            result.errorBound = std::numeric_limits<float>::quiet_NaN();
            result.diagnostics.message = "WebGPU bounds submission was cancelled";
        }

        [[nodiscard]] bool isSignCrossing(float const first,
                                          float const second,
                                          float const threshold) noexcept
        {
            return (first <= threshold && second >= -threshold) ||
                   (second <= threshold && first >= -threshold);
        }

        void addInterpolatedEdgePoint(BoundsAccumulator & accumulator,
                                      compute::BoundsRequest const & request,
                                      std::array<float, 3> const & firstPoint,
                                      std::array<float, 3> const & secondPoint,
                                      float const firstValue,
                                      float const secondValue) noexcept
        {
            auto const threshold = request.probeSettings.surfaceThreshold;
            if (std::abs(firstValue) <= threshold)
            {
                accumulator.add(firstPoint);
                return;
            }
            if (std::abs(secondValue) <= threshold)
            {
                accumulator.add(secondPoint);
                return;
            }

            auto const denominator = secondValue - firstValue;
            float interpolation = 0.5f;
            if (std::isfinite(denominator) &&
                std::abs(denominator) > std::numeric_limits<float>::epsilon())
            {
                interpolation = std::clamp(-firstValue / denominator, 0.0f, 1.0f);
            }

            std::array<float, 3> point{};
            for (std::size_t axis = 0u; axis < 3u; ++axis)
            {
                point[axis] = firstPoint[axis] +
                              interpolation * (secondPoint[axis] - firstPoint[axis]);
            }
            accumulator.add(point);
        }

        [[nodiscard]] compute::BoundsResult determineBounds(
          std::shared_ptr<WebGPUComputeContext> const & context,
          std::shared_ptr<const compute::RenderSceneSnapshot> const & snapshot,
                    compute::BoundsRequest const & request,
                    std::atomic_bool const & cancelRequested)
        {
            auto result = makeResultBase(request);
                        if (cancelRequested.load(std::memory_order_acquire))
                        {
                                cancelResult(result);
                                return result;
                        }

            auto const dimensions = calculateGridDimensions(request);
            if (!dimensions.has_value())
            {
                return makeError(request,
                                 compute::BoundsResultStatus::Invalid,
                                 compute::BoundsErrorCode::InvalidRequest,
                                 "WebGPU bounds probe resolution exceeds the supported tiled limit");
            }

            if (!context || !context->isValid())
            {
                return makeError(request,
                                 compute::BoundsResultStatus::Unavailable,
                                 compute::BoundsErrorCode::Unavailable,
                                 "WebGPU bounds service has no valid compute context");
            }
            if (!snapshot || !snapshot->isValid())
            {
                return makeError(request,
                                 compute::BoundsResultStatus::Invalid,
                                 compute::BoundsErrorCode::NoScene,
                                 "WebGPU bounds service has no valid analytic scene snapshot");
            }

            std::array<float, 3> const extent{
              request.probeDomain.max[0] - request.probeDomain.min[0],
              request.probeDomain.max[1] - request.probeDomain.min[1],
              request.probeDomain.max[2] - request.probeDomain.min[2]};
            if (!std::isfinite(extent[0]) || !std::isfinite(extent[1]) ||
                !std::isfinite(extent[2]) || extent[0] <= 0.0f || extent[1] <= 0.0f ||
                extent[2] <= 0.0f)
            {
                return makeError(request,
                                 compute::BoundsResultStatus::Invalid,
                                 compute::BoundsErrorCode::NoFiniteDomain,
                                 "WebGPU bounds probe domain has a non-finite extent");
            }

            std::vector<float> values(dimensions->count,
                                      std::numeric_limits<float>::quiet_NaN());
            auto const shader = WebGPUSdfShaderComposer::compose(snapshot->analyticEvaluatorWgsl);
            WebGPUSdfEvaluator evaluator{context};

            auto const & resolution = request.probeSettings.resolution;
            auto const & tileSize = request.probeSettings.tileSize;
            for (std::size_t zStart = 0u; zStart < resolution[2]; zStart += tileSize[2])
            {
                if (cancelRequested.load(std::memory_order_acquire))
                {
                    cancelResult(result);
                    return result;
                }
                auto const zEnd = std::min<std::size_t>(resolution[2], zStart + tileSize[2]);
                for (std::size_t yStart = 0u; yStart < resolution[1]; yStart += tileSize[1])
                {
                    if (cancelRequested.load(std::memory_order_acquire))
                    {
                        cancelResult(result);
                        return result;
                    }
                    auto const yEnd = std::min<std::size_t>(resolution[1], yStart + tileSize[1]);
                    for (std::size_t xStart = 0u; xStart < resolution[0]; xStart += tileSize[0])
                    {
                        if (cancelRequested.load(std::memory_order_acquire))
                        {
                            cancelResult(result);
                            return result;
                        }
                        auto const xEnd = std::min<std::size_t>(resolution[0], xStart + tileSize[0]);
                        std::vector<std::array<float, 3>> positions;
                        std::vector<std::size_t> indices;
                        positions.reserve((xEnd - xStart + 1u) * (yEnd - yStart + 1u) *
                                           (zEnd - zStart + 1u));
                        indices.reserve(positions.capacity());

                        for (std::size_t z = zStart; z <= zEnd; ++z)
                        {
                            for (std::size_t y = yStart; y <= yEnd; ++y)
                            {
                                for (std::size_t x = xStart; x <= xEnd; ++x)
                                {
                                    positions.push_back(gridPoint(request.probeDomain,
                                                                  request,
                                                                  x,
                                                                  y,
                                                                  z));
                                    indices.push_back(gridIndex(*dimensions, x, y, z));
                                }
                            }
                        }

                        auto evaluation = evaluator.evaluate(compute::SdfEvaluationRequest{
                          .positions = std::move(positions),
                          .isoValue = request.probeSettings.isoValue,
                          .shaderSource = shader,
                          .parameterValues = snapshot->parameterValues});
                        if (evaluation.values.size() != indices.size())
                        {
                            return makeError(request,
                                             compute::BoundsResultStatus::Failed,
                                             compute::BoundsErrorCode::ReadbackFailed,
                                             "WebGPU bounds probe returned an unexpected sample count");
                        }

                        if (cancelRequested.load(std::memory_order_acquire))
                        {
                            cancelResult(result);
                            return result;
                        }

                        for (std::size_t index = 0u; index < indices.size(); ++index)
                        {
                            values[indices[index]] = evaluation.values[index];
                        }
                        result.diagnostics.probeCount += evaluation.values.size();
                        ++result.diagnostics.tileCount;
                    }
                }
            }

            auto const pointAt = [&](std::size_t const x,
                                     std::size_t const y,
                                     std::size_t const z)
            { return gridPoint(request.probeDomain, request, x, y, z); };

            BoundsAccumulator surfaceBounds;
            BoundsAccumulator negativeSampleBounds;
            auto const threshold = request.probeSettings.surfaceThreshold;
            for (std::size_t z = 0u; z < static_cast<std::size_t>(resolution[2]) + 1u; ++z)
            {
                for (std::size_t y = 0u; y < static_cast<std::size_t>(resolution[1]) + 1u; ++y)
                {
                    for (std::size_t x = 0u; x < static_cast<std::size_t>(resolution[0]) + 1u; ++x)
                    {
                        auto const value = values[gridIndex(*dimensions, x, y, z)];
                        if (!std::isfinite(value))
                        {
                            result.diagnostics.encounteredNonFinite = true;
                            continue;
                        }

                        auto const point = pointAt(x, y, z);
                        if (value < 0.0f)
                        {
                            negativeSampleBounds.add(point);
                        }
                        if (std::abs(value) <= threshold)
                        {
                            surfaceBounds.add(point);
                        }
                    }
                }
            }

            if (result.diagnostics.encounteredNonFinite)
            {
                return makeError(request,
                                 compute::BoundsResultStatus::Invalid,
                                 compute::BoundsErrorCode::NoFiniteDomain,
                                 "WebGPU bounds probe produced a non-finite model distance");
            }

            constexpr std::array<std::array<std::size_t, 2>, 12> EDGES{{
              {{0u, 1u}}, {{0u, 2u}}, {{0u, 4u}}, {{1u, 3u}}, {{1u, 5u}}, {{2u, 3u}},
              {{2u, 6u}}, {{3u, 7u}}, {{4u, 5u}}, {{4u, 6u}}, {{5u, 7u}}, {{6u, 7u}}}};
            constexpr std::array<std::array<std::size_t, 3>, 8> CORNER_OFFSETS{{
              {{0u, 0u, 0u}}, {{1u, 0u, 0u}}, {{0u, 1u, 0u}}, {{1u, 1u, 0u}},
              {{0u, 0u, 1u}}, {{1u, 0u, 1u}}, {{0u, 1u, 1u}}, {{1u, 1u, 1u}}}};

            for (std::size_t z = 0u; z < static_cast<std::size_t>(resolution[2]); ++z)
            {
                for (std::size_t y = 0u; y < static_cast<std::size_t>(resolution[1]); ++y)
                {
                    for (std::size_t x = 0u; x < static_cast<std::size_t>(resolution[0]); ++x)
                    {
                        std::array<float, 8> cornerValues{};
                        std::array<std::array<float, 3>, 8> cornerPoints{};
                        float minimum = std::numeric_limits<float>::max();
                        float maximum = std::numeric_limits<float>::lowest();
                        for (std::size_t corner = 0u; corner < cornerValues.size(); ++corner)
                        {
                            auto const offset = CORNER_OFFSETS[corner];
                            cornerValues[corner] = values[gridIndex(*dimensions,
                                                                    x + offset[0],
                                                                    y + offset[1],
                                                                    z + offset[2])];
                            cornerPoints[corner] = pointAt(x + offset[0],
                                                           y + offset[1],
                                                           z + offset[2]);
                            minimum = std::min(minimum, cornerValues[corner]);
                            maximum = std::max(maximum, cornerValues[corner]);
                        }

                        if (minimum > threshold || maximum < -threshold)
                        {
                            continue;
                        }

                        for (auto const & edge : EDGES)
                        {
                            auto const first = edge[0];
                            auto const second = edge[1];
                            if (isSignCrossing(cornerValues[first], cornerValues[second], threshold))
                            {
                                addInterpolatedEdgePoint(surfaceBounds,
                                                         request,
                                                         cornerPoints[first],
                                                         cornerPoints[second],
                                                         cornerValues[first],
                                                         cornerValues[second]);
                                ++result.diagnostics.projectedCount;
                            }
                        }
                    }
                }
            }

            auto const stepX = extent[0] / static_cast<float>(resolution[0]);
            auto const stepY = extent[1] / static_cast<float>(resolution[1]);
            auto const stepZ = extent[2] / static_cast<float>(resolution[2]);
            auto const cellDiagonal = std::sqrt(stepX * stepX + stepY * stepY + stepZ * stepZ);
            if (!surfaceBounds.hasValue && negativeSampleBounds.hasValue)
            {
                surfaceBounds.add(negativeSampleBounds.bounds.expanded(cellDiagonal));
            }

            if (!surfaceBounds.hasValue || !surfaceBounds.bounds.isValid())
            {
                if (negativeSampleBounds.hasValue)
                {
                    result.status = compute::BoundsResultStatus::Failed;
                    result.errorCode = compute::BoundsErrorCode::DomainClipped;
                    result.diagnostics.touchedProbeDomain = true;
                    result.diagnostics.message =
                      "WebGPU bounds probe found model samples but no contained surface";
                    return result;
                }

                result.status = compute::BoundsResultStatus::Empty;
                result.errorCode = compute::BoundsErrorCode::NoSurface;
                result.diagnostics.message = "WebGPU bounds probe found no surface";
                return result;
            }

            result.modelBounds = surfaceBounds.bounds;
            result.source = compute::BoundsSource::GpuProbe;
            result.guarantee = compute::BoundsGuarantee::NumericalOpenClParity;
            result.errorBound = cellDiagonal + threshold;
            result.touchesProbeDomain = [&]()
            {
                auto const tolerance = result.errorBound;
                return result.modelBounds->min[0] <= request.probeDomain.min[0] + tolerance ||
                       result.modelBounds->min[1] <= request.probeDomain.min[1] + tolerance ||
                       result.modelBounds->min[2] <= request.probeDomain.min[2] + tolerance ||
                       result.modelBounds->max[0] >= request.probeDomain.max[0] - tolerance ||
                       result.modelBounds->max[1] >= request.probeDomain.max[1] - tolerance ||
                       result.modelBounds->max[2] >= request.probeDomain.max[2] - tolerance;
            }();
            result.diagnostics.touchedProbeDomain = result.touchesProbeDomain;
            result.authoritative = !result.touchesProbeDomain;
            result.status = result.touchesProbeDomain ? compute::BoundsResultStatus::Failed
                                                       : compute::BoundsResultStatus::Ready;
            if (result.touchesProbeDomain)
            {
                result.errorCode = compute::BoundsErrorCode::DomainClipped;
                result.diagnostics.message =
                  "WebGPU bounds probe reached the configured evaluation domain";
            }
            else
            {
                result.diagnostics.acceptedCount = result.diagnostics.projectedCount;
            }

            if (request.probeSettings.requireConservativeResult)
            {
                result.status = compute::BoundsResultStatus::Failed;
                result.errorCode = compute::BoundsErrorCode::UnsupportedGraph;
                result.authoritative = false;
                result.diagnostics.message =
                  "WebGPU numerical bounds do not satisfy the requested conservative guarantee";
            }
            return result;
        }

        class WebGPUBoundsSubmission final : public compute::IBoundsSubmission
        {
          public:
            WebGPUBoundsSubmission(std::future<compute::BoundsResult> future,
                                   std::shared_ptr<const compute::RenderSceneSnapshot> snapshot,
                                   std::function<void(compute::BoundsResult const &)> publish,
                                   std::shared_ptr<std::atomic_bool> cancelRequested)
                : m_future{std::move(future)}
                , m_snapshot{std::move(snapshot)}
                , m_publish{std::move(publish)}
                , m_cancelRequested{std::move(cancelRequested)}
            {
            }

            explicit WebGPUBoundsSubmission(compute::BoundsResult result)
                : m_status{compute::BoundsSubmissionStatus::Failed}
                , m_result{std::move(result)}
            {
            }

            ~WebGPUBoundsSubmission() override
            {
                if (getStatus() == compute::BoundsSubmissionStatus::Pending)
                {
                    try
                    {
                        wait();
                    }
                    catch (...)
                    {
                    }
                }
            }

            void progress() noexcept override
            {
                std::scoped_lock lock{m_mutex};
                pollLocked(false);
            }

            [[nodiscard]] compute::BoundsSubmissionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_mutex};
                const_cast<WebGPUBoundsSubmission *>(this)->pollLocked(false);
                return m_status;
            }

            void requestCancellation() noexcept override
            {
                if (m_cancelRequested)
                {
                    m_cancelRequested->store(true, std::memory_order_release);
                }
            }

            void wait() override
            {
                std::scoped_lock lock{m_mutex};
                pollLocked(true);
            }

            [[nodiscard]] std::optional<compute::BoundsResult> takeResult() override
            {
                std::scoped_lock lock{m_mutex};
                pollLocked(false);
                if (m_status == compute::BoundsSubmissionStatus::Pending)
                {
                    return std::nullopt;
                }
                return std::exchange(m_result, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                std::scoped_lock lock{m_mutex};
                return m_errorMessage;
            }

          private:
            void pollLocked(bool const block)
            {
                if (m_status != compute::BoundsSubmissionStatus::Pending || !m_future.valid())
                {
                    return;
                }
                if (!block &&
                    m_future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
                {
                    return;
                }

                try
                {
                    auto result = m_future.get();
                    if (m_cancelRequested &&
                        m_cancelRequested->load(std::memory_order_acquire))
                    {
                        cancelResult(result);
                        m_result = std::move(result);
                        m_status = compute::BoundsSubmissionStatus::Cancelled;
                    }
                    else
                    {
                        m_result = std::move(result);
                        if (m_result->status == compute::BoundsResultStatus::Cancelled)
                        {
                            m_errorMessage = m_result->diagnostics.message;
                            m_status = compute::BoundsSubmissionStatus::Cancelled;
                            return;
                        }
                        if (m_result->status != compute::BoundsResultStatus::Ready)
                        {
                            m_errorMessage = m_result->diagnostics.message;
                        }
                        if (m_publish && m_result->status == compute::BoundsResultStatus::Ready)
                        {
                            m_publish(*m_result);
                        }
                        m_status = compute::BoundsSubmissionStatus::Succeeded;
                    }
                }
                catch (std::exception const & error)
                {
                    m_status = compute::BoundsSubmissionStatus::Failed;
                    m_errorMessage = error.what();
                }
                catch (...)
                {
                    m_status = compute::BoundsSubmissionStatus::Failed;
                    m_errorMessage = "WebGPU bounds submission failed";
                }
            }

            mutable std::mutex m_mutex;
            std::future<compute::BoundsResult> m_future;
            std::shared_ptr<const compute::RenderSceneSnapshot> m_snapshot;
            std::function<void(compute::BoundsResult const &)> m_publish;
            std::shared_ptr<std::atomic_bool> m_cancelRequested;
            mutable compute::BoundsSubmissionStatus m_status{
              compute::BoundsSubmissionStatus::Pending};
            std::optional<compute::BoundsResult> m_result;
            std::string m_errorMessage;
        };
    }

    WebGPUBoundsService::WebGPUBoundsService(std::shared_ptr<WebGPUComputeContext> context)
        : m_context{std::move(context)}
        , m_state{std::make_shared<State>()}
    {
    }

    compute::RendererCapability WebGPUBoundsService::getCapabilities() const noexcept
    {
        return compute::RendererCapability::BoundingBoxDetermination;
    }

    bool WebGPUBoundsService::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    void WebGPUBoundsService::setSceneSnapshot(
      std::shared_ptr<const compute::RenderSceneSnapshot> snapshot) noexcept
    {
        std::scoped_lock lock{m_state->mutex};
        m_state->snapshot = std::move(snapshot);
    }

    std::optional<compute::BoundsResult>
    WebGPUBoundsService::getCachedResult(
      compute::RenderFreshnessStamp const & freshness) const noexcept
    {
        std::scoped_lock lock{m_state->mutex};
        if (!m_state->cachedResult.has_value() ||
            !m_state->cachedResult->isCurrentFor(freshness))
        {
            return std::nullopt;
        }
        return m_state->cachedResult;
    }

    std::unique_ptr<compute::IBoundsSubmission>
    WebGPUBoundsService::submit(compute::BoundsRequest request)
    {
        if (!request.isValid())
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Invalid,
              compute::BoundsErrorCode::InvalidRequest,
              "WebGPU bounds request is invalid"));
        }
        if (!isAvailable())
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Unavailable,
              compute::BoundsErrorCode::Unavailable,
              "WebGPU bounds service is unavailable"));
        }

        std::shared_ptr<const compute::RenderSceneSnapshot> snapshot;
        {
            std::scoped_lock lock{m_state->mutex};
            snapshot = m_state->snapshot;
        }
        if (!snapshot)
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Invalid,
              compute::BoundsErrorCode::NoScene,
              "WebGPU bounds service has no published scene snapshot"));
        }
        if (snapshot->sceneGeneration != request.freshness.sceneGeneration)
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Stale,
              compute::BoundsErrorCode::Stale,
              "WebGPU bounds request does not match the published scene snapshot"));
        }
        if (!compute::hasCapability(snapshot->requiredCapabilities,
                                   compute::RendererCapability::AnalyticRendering))
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Invalid,
              compute::BoundsErrorCode::UnsupportedGraph,
              "WebGPU bounds currently supports analytic scene snapshots only"));
        }

        try
        {
            auto const state = m_state;
            auto cancelRequested = std::make_shared<std::atomic_bool>(false);
            auto future = std::async(std::launch::async,
                                     [context = m_context, snapshot, request, cancelRequested]()
                                     {
                                         return determineBounds(
                                           context, snapshot, request, *cancelRequested);
                                     });
            auto publish = [state](compute::BoundsResult const & result)
            {
                std::scoped_lock lock{state->mutex};
                state->cachedResult = result;
            };
            return std::make_unique<WebGPUBoundsSubmission>(std::move(future),
                                                              std::move(snapshot),
                                                              std::move(publish),
                                                              std::move(cancelRequested));
        }
        catch (std::exception const & error)
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Failed,
              compute::BoundsErrorCode::DispatchFailed,
              std::string{"Unable to submit WebGPU bounds work: "} + error.what()));
        }
        catch (...)
        {
            return std::make_unique<WebGPUBoundsSubmission>(makeError(
              request,
              compute::BoundsResultStatus::Failed,
              compute::BoundsErrorCode::DispatchFailed,
              "Unable to submit WebGPU bounds work"));
        }
    }
}
