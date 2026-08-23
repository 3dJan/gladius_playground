#include "compute/OpenCLBoundsService.h"

#include "compute/ComputeCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace gladius::compute
{
    struct OpenCLBoundsService::State
    {
        mutable std::mutex mutex;
        std::shared_ptr<const RenderSceneSnapshot> snapshot;
        std::optional<BoundsResult> cachedResult;
    };

    namespace
    {
        [[nodiscard]] BoundsResult makeResultBase(BoundsRequest const & request)
        {
            BoundsResult result;
            result.probeDomain = request.probeDomain;
            result.freshness = request.freshness;
            result.diagnostics.probeCount = static_cast<std::uint64_t>(request.probeSettings.resolution[0]) *
                                             static_cast<std::uint64_t>(request.probeSettings.resolution[1]) *
                                             static_cast<std::uint64_t>(request.probeSettings.resolution[2]);
            result.diagnostics.tileCount =
              ((request.probeSettings.resolution[0] + request.probeSettings.tileSize[0] - 1u) /
               request.probeSettings.tileSize[0]) *
              static_cast<std::uint64_t>((request.probeSettings.resolution[1] +
                                          request.probeSettings.tileSize[1] - 1u) /
                                         request.probeSettings.tileSize[1]) *
              static_cast<std::uint64_t>((request.probeSettings.resolution[2] +
                                          request.probeSettings.tileSize[2] - 1u) /
                                         request.probeSettings.tileSize[2]);
            return result;
        }

        [[nodiscard]] BoundsResult makeError(BoundsRequest const & request,
                                              BoundsResultStatus const status,
                                              BoundsErrorCode const errorCode,
                                              std::string message)
        {
            auto result = makeResultBase(request);
            result.status = status;
            result.errorCode = errorCode;
            result.diagnostics.message = std::move(message);
            return result;
        }

        [[nodiscard]] RenderBounds convertBounds(BoundingBox const & box) noexcept
        {
            return {.min = {box.min.x, box.min.y, box.min.z},
                    .max = {box.max.x, box.max.y, box.max.z}};
        }

        [[nodiscard]] bool touchesDomain(RenderBounds const & bounds,
                                         RenderEvaluationDomain const & domain) noexcept
        {
            return bounds.min[0] <= domain.min[0] || bounds.min[1] <= domain.min[1] ||
                   bounds.min[2] <= domain.min[2] || bounds.max[0] >= domain.max[0] ||
                   bounds.max[1] >= domain.max[1] || bounds.max[2] >= domain.max[2];
        }

        [[nodiscard]] BoundsResult computeBounds(std::shared_ptr<gladius::ComputeCore> const & core,
                                                 BoundsRequest const & request)
        {
            auto result = makeResultBase(request);
            if (!core)
            {
                return makeError(request,
                                 BoundsResultStatus::Unavailable,
                                 BoundsErrorCode::Unavailable,
                                 "OpenCL bounds service has no ComputeCore");
            }

            try
            {
                auto context = core->getComputeContext();
                if (!context || !context->isValid())
                {
                    return makeError(request,
                                     BoundsResultStatus::Unavailable,
                                     BoundsErrorCode::Unavailable,
                                     "OpenCL compute context is unavailable");
                }

                if (core->isBoundingBoxStale())
                {
                    core->recomputeStaleBoundingBox();
                }

                if (!core->updateBBox())
                {
                    return makeError(request,
                                     BoundsResultStatus::Failed,
                                     BoundsErrorCode::DispatchFailed,
                                     "OpenCL bounds probe did not complete successfully");
                }

                auto const box = core->getBoundingBox();
                if (!box.has_value())
                {
                    return makeError(request,
                                     BoundsResultStatus::Empty,
                                     BoundsErrorCode::NoSurface,
                                     "OpenCL bounds probe found no surface");
                }

                auto const bounds = convertBounds(*box);
                auto const source = core->getBoundingBoxComputationSource();
                result.modelBounds = bounds;
                result.touchesProbeDomain = touchesDomain(bounds, request.probeDomain);
                result.diagnostics.touchedProbeDomain = result.touchesProbeDomain;

                switch (source)
                {
                case gladius::BoundingBoxComputationSource::NumericalProbe:
                    result.status = BoundsResultStatus::Ready;
                    result.source = BoundsSource::GpuProbe;
                    result.guarantee = BoundsGuarantee::NumericalOpenClParity;
                    result.authoritative = bounds.isValid() && !result.touchesProbeDomain;
                    result.errorBound = request.probeSettings.residualTolerance;
                    result.diagnostics.acceptedCount = result.authoritative ? 1u : 0u;
                    if (result.touchesProbeDomain)
                    {
                        result.status = BoundsResultStatus::Failed;
                        result.errorCode = BoundsErrorCode::DomainClipped;
                        result.authoritative = false;
                        result.diagnostics.message =
                          "OpenCL bounds probe reached the configured evaluation domain";
                    }
                    else if (!bounds.isValid())
                    {
                        result.status = BoundsResultStatus::Invalid;
                        result.errorCode = BoundsErrorCode::NoSurface;
                        result.authoritative = false;
                        result.diagnostics.message = "OpenCL bounds probe produced a degenerate box";
                    }
                    break;

                case gladius::BoundingBoxComputationSource::PrimitiveMetadata:
                    result.status = BoundsResultStatus::Ready;
                    result.source = BoundsSource::ResourceMetadata;
                    result.guarantee = BoundsGuarantee::ExactMetadata;
                    result.authoritative = bounds.isValid() && !result.touchesProbeDomain;
                    result.errorBound = 0.0f;
                    result.diagnostics.usedMetadataFallback = true;
                    result.diagnostics.acceptedCount = result.authoritative ? 1u : 0u;
                    if (!result.authoritative)
                    {
                        result.status = BoundsResultStatus::Invalid;
                        result.errorCode = result.touchesProbeDomain ? BoundsErrorCode::DomainClipped
                                                                     : BoundsErrorCode::InvalidResource;
                        result.diagnostics.message =
                          "OpenCL primitive metadata did not provide a finite positive model box";
                    }
                    break;

                case gladius::BoundingBoxComputationSource::BuildVolumeFallback:
                    result.status = BoundsResultStatus::Ready;
                    result.source = BoundsSource::BuildVolumeFallback;
                    result.guarantee = BoundsGuarantee::BuildVolumeFallback;
                    result.authoritative = false;
                    result.errorBound = std::numeric_limits<float>::quiet_NaN();
                    result.diagnostics.usedMetadataFallback = true;
                    result.diagnostics.message =
                      "OpenCL bounds used the build volume because no authoritative model bounds were found";
                    break;

                case gladius::BoundingBoxComputationSource::None:
                default:
                    result.status = BoundsResultStatus::Invalid;
                    result.errorCode = BoundsErrorCode::Internal;
                    result.modelBounds.reset();
                    result.diagnostics.message =
                      "OpenCL bounds completed without a recognized result provenance";
                    break;
                }

                if (request.probeSettings.requireConservativeResult &&
                    result.guarantee != BoundsGuarantee::CertifiedConservative &&
                    result.guarantee != BoundsGuarantee::ExactMetadata)
                {
                    result.status = BoundsResultStatus::Failed;
                    result.errorCode = BoundsErrorCode::UnsupportedGraph;
                    result.authoritative = false;
                    result.diagnostics.message =
                      "OpenCL numerical bounds do not satisfy the requested conservative guarantee";
                }
                return result;
            }
            catch (std::exception const & error)
            {
                return makeError(request,
                                 BoundsResultStatus::Failed,
                                 BoundsErrorCode::Internal,
                                 std::string{"OpenCL bounds determination failed: "} + error.what());
            }
            catch (...)
            {
                return makeError(request,
                                 BoundsResultStatus::Failed,
                                 BoundsErrorCode::Internal,
                                 "OpenCL bounds determination failed");
            }
        }

        class OpenCLBoundsSubmission final : public IBoundsSubmission
        {
          public:
            OpenCLBoundsSubmission(std::future<BoundsResult> future,
                                   std::shared_ptr<const RenderSceneSnapshot> snapshot,
                                   std::function<void(BoundsResult const &)> publish)
                : m_future{std::move(future)}
                , m_snapshot{std::move(snapshot)}
                , m_publish{std::move(publish)}
            {
            }

            explicit OpenCLBoundsSubmission(BoundsResult result)
                : m_status{BoundsSubmissionStatus::Failed}
                , m_result{std::move(result)}
            {
            }

            ~OpenCLBoundsSubmission() override
            {
                if (getStatus() == BoundsSubmissionStatus::Pending)
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

            [[nodiscard]] BoundsSubmissionStatus getStatus() const noexcept override
            {
                std::scoped_lock lock{m_mutex};
                const_cast<OpenCLBoundsSubmission *>(this)->pollLocked(false);
                return m_status;
            }

            void requestCancellation() noexcept override
            {
                m_cancelRequested.store(true, std::memory_order_release);
            }

            void wait() override
            {
                std::scoped_lock lock{m_mutex};
                pollLocked(true);
            }

            [[nodiscard]] std::optional<BoundsResult> takeResult() override
            {
                std::scoped_lock lock{m_mutex};
                pollLocked(false);
                if (m_status == BoundsSubmissionStatus::Pending)
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
                if (m_status != BoundsSubmissionStatus::Pending || !m_future.valid())
                {
                    return;
                }

                if (!block && m_future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
                {
                    return;
                }

                try
                {
                    auto result = m_future.get();
                    if (m_cancelRequested.load(std::memory_order_acquire))
                    {
                        result.status = BoundsResultStatus::Cancelled;
                        result.source = BoundsSource::None;
                        result.guarantee = BoundsGuarantee::None;
                        result.errorCode = BoundsErrorCode::Cancelled;
                        result.modelBounds.reset();
                        result.authoritative = false;
                        result.errorBound = std::numeric_limits<float>::quiet_NaN();
                        result.diagnostics.message = "OpenCL bounds submission was cancelled";
                        m_result = std::move(result);
                        m_status = BoundsSubmissionStatus::Cancelled;
                        return;
                    }

                    m_result = std::move(result);
                    if (m_result->status == BoundsResultStatus::Failed ||
                        m_result->status == BoundsResultStatus::Unavailable ||
                        m_result->status == BoundsResultStatus::Invalid ||
                        m_result->status == BoundsResultStatus::Empty)
                    {
                        m_errorMessage = m_result->diagnostics.message;
                    }
                    if (m_publish && m_result->status != BoundsResultStatus::Cancelled)
                    {
                        m_publish(*m_result);
                    }
                    m_status = BoundsSubmissionStatus::Succeeded;
                }
                catch (std::exception const & error)
                {
                    m_status = BoundsSubmissionStatus::Failed;
                    m_errorMessage = error.what();
                }
                catch (...)
                {
                    m_status = BoundsSubmissionStatus::Failed;
                    m_errorMessage = "OpenCL bounds submission failed";
                }
            }

            mutable std::mutex m_mutex;
            std::future<BoundsResult> m_future;
            std::shared_ptr<const RenderSceneSnapshot> m_snapshot;
            std::function<void(BoundsResult const &)> m_publish;
            std::atomic_bool m_cancelRequested{false};
            mutable BoundsSubmissionStatus m_status{BoundsSubmissionStatus::Pending};
            std::optional<BoundsResult> m_result;
            std::string m_errorMessage;
        };
    }

    OpenCLBoundsService::OpenCLBoundsService(std::shared_ptr<gladius::ComputeCore> core)
        : m_core{std::move(core)}
        , m_state{std::make_shared<State>()}
    {
    }

    RendererCapability OpenCLBoundsService::getCapabilities() const noexcept
    {
        return RendererCapability::BoundingBoxDetermination;
    }

    bool OpenCLBoundsService::isAvailable() const noexcept
    {
        if (!m_core)
        {
            return false;
        }

        try
        {
            auto context = m_core->getComputeContext();
            return context && context->isValid();
        }
        catch (...)
        {
            return false;
        }
    }

    void OpenCLBoundsService::setSceneSnapshot(
      std::shared_ptr<const RenderSceneSnapshot> snapshot) noexcept
    {
        std::scoped_lock lock{m_state->mutex};
        m_state->snapshot = std::move(snapshot);
    }

    std::optional<BoundsResult>
    OpenCLBoundsService::getCachedResult(RenderFreshnessStamp const & freshness) const noexcept
    {
        std::scoped_lock lock{m_state->mutex};
        if (!m_state->cachedResult.has_value() || !m_state->cachedResult->isCurrentFor(freshness))
        {
            return std::nullopt;
        }
        return m_state->cachedResult;
    }

    std::unique_ptr<IBoundsSubmission> OpenCLBoundsService::submit(BoundsRequest request)
    {
        if (!request.isValid())
        {
            return std::make_unique<OpenCLBoundsSubmission>(
              makeError(request,
                        BoundsResultStatus::Invalid,
                        BoundsErrorCode::InvalidRequest,
                        "OpenCL bounds request is invalid"));
        }
        if (!isAvailable())
        {
            return std::make_unique<OpenCLBoundsSubmission>(
              makeError(request,
                        BoundsResultStatus::Unavailable,
                        BoundsErrorCode::Unavailable,
                        "OpenCL bounds service is unavailable"));
        }

        std::shared_ptr<const RenderSceneSnapshot> snapshot;
        {
            std::scoped_lock lock{m_state->mutex};
            snapshot = m_state->snapshot;
        }
        if (!snapshot)
        {
            return std::make_unique<OpenCLBoundsSubmission>(
              makeError(request,
                        BoundsResultStatus::Invalid,
                        BoundsErrorCode::NoScene,
                        "OpenCL bounds service has no published scene snapshot"));
        }
        if (snapshot->sceneGeneration != request.freshness.sceneGeneration)
        {
            return std::make_unique<OpenCLBoundsSubmission>(
              makeError(request,
                        BoundsResultStatus::Stale,
                        BoundsErrorCode::Stale,
                        "OpenCL bounds request does not match the published scene snapshot"));
        }

        try
        {
            auto const state = m_state;
            auto future = std::async(std::launch::async,
                                     [core = m_core, request]()
                                     { return computeBounds(core, request); });
            auto publish = [state](BoundsResult const & result)
            {
                std::scoped_lock lock{state->mutex};
                state->cachedResult = result;
            };
            return std::make_unique<OpenCLBoundsSubmission>(std::move(future),
                                                              std::move(snapshot),
                                                              std::move(publish));
        }
        catch (std::exception const & error)
        {
            auto result = makeError(request,
                                    BoundsResultStatus::Failed,
                                    BoundsErrorCode::DispatchFailed,
                                    std::string{"Unable to submit OpenCL bounds work: "} + error.what());
            return std::make_unique<OpenCLBoundsSubmission>(std::move(result));
        }
        catch (...)
        {
            return std::make_unique<OpenCLBoundsSubmission>(
              makeError(request,
                        BoundsResultStatus::Failed,
                        BoundsErrorCode::DispatchFailed,
                        "Unable to submit OpenCL bounds work"));
        }
    }
}
