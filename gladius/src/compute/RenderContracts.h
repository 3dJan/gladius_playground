#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <limits>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief Backend-neutral feature flags exposed by a physical renderer.
     */
    enum class RendererCapability : std::uint32_t
    {
        None = 0u,
        AnalyticRendering = 1u << 0u,
        ProgressiveRendering = 1u << 1u,
        LowResolutionPreview = 1u << 2u,
        PrecomputedSdf = 1u << 3u,
        MeshSdf = 1u << 4u,
        BeamLattice = 1u << 5u,
        ImageSampling = 1u << 6u,
        VdbSampling = 1u << 7u,
        Contours = 1u << 8u,
        Export = 1u << 9u,
        FramePresentation = 1u << 10u,
    };

    [[nodiscard]] constexpr RendererCapability operator|(RendererCapability const left,
                                                          RendererCapability const right) noexcept
    {
        return static_cast<RendererCapability>(static_cast<std::uint32_t>(left) |
                                                static_cast<std::uint32_t>(right));
    }

    [[nodiscard]] constexpr bool hasCapability(RendererCapability const capabilities,
                                               RendererCapability const required) noexcept
    {
        return (static_cast<std::uint32_t>(capabilities) & static_cast<std::uint32_t>(required)) ==
               static_cast<std::uint32_t>(required);
    }

    /**
     * @brief Immutable camera basis used by both OpenCL and WebGPU render adapters.
     */
    struct RenderCamera
    {
        std::array<float, 3> eyePosition{0.0f, 0.0f, 2.0f};
        std::array<float, 3> forwardDirection{0.0f, 0.0f, -1.0f};
        std::array<float, 3> rightDirection{1.0f, 0.0f, 0.0f};
        std::array<float, 3> upDirection{0.0f, 1.0f, 0.0f};

        [[nodiscard]] bool isValid() const noexcept
        {
            return hasFiniteComponents(eyePosition) && hasNonZeroFiniteComponents(forwardDirection) &&
                   hasNonZeroFiniteComponents(rightDirection) && hasNonZeroFiniteComponents(upDirection);
        }

      private:
        [[nodiscard]] static bool hasFiniteComponents(std::array<float, 3> const & value) noexcept
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
        }

        [[nodiscard]] static bool hasNonZeroFiniteComponents(std::array<float, 3> const & value) noexcept
        {
            auto const squaredLength = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
            return hasFiniteComponents(value) && std::isfinite(squaredLength) && squaredLength > 0.0f;
        }
    };

    /**
     * @brief Backend-neutral rendering mode corresponding to the current user-visible quality path.
     */
    enum class RenderMode
    {
        FullModel,
        Hybrid,
        PrecomputedSdf,
        DistanceInitialized,
    };

    /**
     * @brief Copy of the rendering values consumed by a single backend dispatch.
     */
    struct RenderSettingsSnapshot
    {
        float timeSeconds{};
        float sliceHeight{};
        std::uint32_t flags{};
        RenderMode mode{RenderMode::FullModel};
        float quality{1.0f};
        float normalOffset{0.001f};
        float meshEarlyExitDistanceSquared{};
        float meshInflationDistance{};
        float meshFwnBeta{4.0f};
        float meshFwnFarFieldFactor{};
        float weightDistanceToNeighbor{};
        float weightMidpoint{};
        std::uint32_t maxRaySteps{2000u};
        float maxTravelDistance{100000.0f};

        [[nodiscard]] bool isValid() const noexcept
        {
            return std::isfinite(timeSeconds) && std::isfinite(sliceHeight) && std::isfinite(quality) &&
                   quality >= 0.0f && std::isfinite(normalOffset) && normalOffset > 0.0f &&
                   std::isfinite(meshEarlyExitDistanceSquared) && meshEarlyExitDistanceSquared >= 0.0f &&
                   std::isfinite(meshInflationDistance) && std::isfinite(meshFwnBeta) && meshFwnBeta > 0.0f &&
                   std::isfinite(meshFwnFarFieldFactor) && meshFwnFarFieldFactor >= 0.0f &&
                   std::isfinite(weightDistanceToNeighbor) && std::isfinite(weightMidpoint) &&
                   maxRaySteps > 0u && std::isfinite(maxTravelDistance) && maxTravelDistance > 0.0f;
        }
    };

    /**
     * @brief Camera-space view-plane scales used to construct primary ray directions.
     *
     * The OpenCL renderer uses a horizontal scale of 0.5 and a vertical scale of
     * 0.5 / aspect. Keeping both values explicit preserves that established projection
     * when a backend-neutral request is adapted by WebGPU.
     */
    struct RenderFrustum
    {
        float horizontalScale{0.5f};
        float verticalScale{0.5f};

        [[nodiscard]] bool isValid() const noexcept
        {
            return std::isfinite(horizontalScale) && horizontalScale > 0.0f &&
                   std::isfinite(verticalScale) && verticalScale > 0.0f;
        }
    };

    /**
     * @brief Viewport extent and inclusive-exclusive row range for a progressive render submission.
     */
    struct RenderViewport
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t firstRow{};
        std::uint32_t endRow{};

        [[nodiscard]] bool isValid() const noexcept
        {
            return width > 0u && height > 0u && firstRow < endRow && endRow <= height;
        }

        [[nodiscard]] std::size_t pixelCount() const noexcept
        {
            if (!isValid())
            {
                return 0u;
            }

            auto const rowCount = static_cast<std::size_t>(endRow - firstRow);
            auto const pixelCount = static_cast<std::size_t>(width) * rowCount;
            return pixelCount / width == rowCount ? pixelCount : 0u;
        }
    };

    /**
     * @brief Values used to reject results produced for stale model, view, or parameter state.
     */
    struct RenderFreshnessStamp
    {
        std::uint64_t sceneGeneration{};
        std::uint64_t viewGeneration{};
        std::uint64_t parameterGeneration{};

        auto operator<=>(RenderFreshnessStamp const &) const = default;
    };

    /**
     * @brief API-neutral immutable request for a frame or progressive row range.
     */
    struct RenderRequest
    {
        RenderCamera camera{};
        RenderFrustum frustum{};
        RenderSettingsSnapshot settings{};
        RenderViewport viewport{};
        RenderFreshnessStamp freshness{};

        [[nodiscard]] bool isValid() const noexcept
        {
            return camera.isValid() && frustum.isValid() && settings.isValid() && viewport.isValid();
        }
    };

    /**
     * @brief Packed RGBA8 rows returned by a completed renderer submission.
     */
    struct RenderFrame
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t firstRow{};
        std::uint32_t endRow{};
        RenderFreshnessStamp freshness{};
        std::vector<std::uint32_t> pixels;

        [[nodiscard]] bool isValid() const noexcept
        {
            RenderViewport const viewport{.width = width,
                                          .height = height,
                                          .firstRow = firstRow,
                                          .endRow = endRow};
            return viewport.isValid() && pixels.size() == viewport.pixelCount();
        }
    };
}
