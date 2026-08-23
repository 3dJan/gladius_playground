#pragma once

#include "compute/RenderContracts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace gladius::compute
{
    /**
     * @brief State of a model-bounds result, independent of submission lifetime.
     */
    enum class BoundsResultStatus
    {
        Pending,
        Ready,
        Stale,
        Empty,
        Invalid,
        Unavailable,
        Failed,
        Cancelled,
    };

    /**
     * @brief Physical or semantic source of a bounds result.
     */
    enum class BoundsSource
    {
        None,
        StaticGraph,
        ResourceMetadata,
        GpuProbe,
        CertifiedAdaptive,
        BuildVolumeFallback,
    };

    /**
     * @brief Guarantee provided by the bounds calculation.
     */
    enum class BoundsGuarantee
    {
        None,
        NumericalOpenClParity,
        ExactMetadata,
        CertifiedConservative,
        Approximate,
        BuildVolumeFallback,
    };

    /**
     * @brief Stable classification for failures and non-ready outcomes.
     */
    enum class BoundsErrorCode
    {
        None,
        InvalidRequest,
        NoScene,
        NoSurface,
        UnsupportedGraph,
        UnsupportedResource,
        InvalidResource,
        NoFiniteDomain,
        DomainClipped,
        DispatchFailed,
        ReadbackFailed,
        DeviceLost,
        Timeout,
        Cancelled,
        Stale,
        Unavailable,
        Internal,
    };

    /**
     * @brief Terminal state of a physical bounds submission.
     */
    enum class BoundsSubmissionStatus
    {
        Pending,
        Succeeded,
        Cancelled,
        Failed,
    };

    /**
     * @brief OpenCL-compatible sampling and projection policy for a bounds request.
     */
    struct BoundsProbeSettings
    {
        std::array<std::uint32_t, 3> resolution{256u, 256u, 256u};
        std::array<std::uint32_t, 3> tileSize{64u, 64u, 64u};
        float isoValue{};
        float surfaceThreshold{0.0001f};
        float maxTravelDistance{20.0f};
        std::uint32_t maxIterations{30u};
        float minStepSize{0.0001f};
        float initialSdfRejection{50.0f};
        float residualTolerance{0.01f};
        bool allowMetadataFallback{true};
        bool requireConservativeResult{};

        [[nodiscard]] bool isValid() const noexcept
        {
            auto const hasPositiveExtent = [](std::array<std::uint32_t, 3> const & value)
            {
                return value[0] > 0u && value[1] > 0u && value[2] > 0u;
            };

            return hasPositiveExtent(resolution) && hasPositiveExtent(tileSize) &&
                   std::isfinite(isoValue) && std::isfinite(surfaceThreshold) &&
                   surfaceThreshold > 0.0f && std::isfinite(maxTravelDistance) &&
                   maxTravelDistance > 0.0f && maxIterations > 0u && std::isfinite(minStepSize) &&
                   minStepSize > 0.0f && std::isfinite(initialSdfRejection) &&
                   initialSdfRejection > 0.0f && std::isfinite(residualTolerance) &&
                   residualTolerance >= 0.0f;
        }
    };

    /**
     * @brief Immutable request for model-bounds determination.
     */
    struct BoundsRequest
    {
        RenderFreshnessStamp freshness{};
        RenderEvaluationDomain probeDomain{.min = {-400.0f, -400.0f, -400.0f},
                                          .max = {800.0f, 800.0f, 800.0f}};
        BoundsProbeSettings probeSettings{};

        [[nodiscard]] bool isValid() const noexcept
        {
            return freshness.sceneGeneration != 0u && probeDomain.isValid() &&
                   probeSettings.isValid();
        }
    };

    /**
     * @brief Compact diagnostics collected while determining bounds.
     */
    struct BoundsDiagnostics
    {
        std::uint64_t probeCount{};
        std::uint64_t projectedCount{};
        std::uint64_t acceptedCount{};
        std::uint64_t tileCount{};
        std::uint32_t refinementDepth{};
        bool encounteredNonFinite{};
        bool touchedProbeDomain{};
        bool usedMetadataFallback{};
        std::string message;
    };

    /**
     * @brief Backend-neutral result for one bounds request.
     */
    struct BoundsResult
    {
        BoundsResultStatus status{BoundsResultStatus::Pending};
        std::optional<RenderBounds> modelBounds{};
        RenderEvaluationDomain probeDomain{};
        RenderFreshnessStamp freshness{};
        BoundsSource source{BoundsSource::None};
        BoundsGuarantee guarantee{BoundsGuarantee::None};
        BoundsErrorCode errorCode{BoundsErrorCode::None};
        bool authoritative{};
        bool touchesProbeDomain{};
        float errorBound{std::numeric_limits<float>::quiet_NaN()};
        BoundsDiagnostics diagnostics{};

        [[nodiscard]] bool isUsable() const noexcept
        {
            return status == BoundsResultStatus::Ready && authoritative &&
                   source != BoundsSource::BuildVolumeFallback && modelBounds.has_value() &&
                   modelBounds->isValid() && std::isfinite(errorBound) && errorBound >= 0.0f;
        }

        [[nodiscard]] bool isCurrentFor(RenderFreshnessStamp const & requested) const noexcept
        {
            return isUsable() && freshness.hasSameModelGeneration(requested);
        }

        [[nodiscard]] bool isTerminal() const noexcept
        {
            return status != BoundsResultStatus::Pending && status != BoundsResultStatus::Stale;
        }
    };
}