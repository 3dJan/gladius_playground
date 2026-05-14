#pragma once

#include <cstddef>
#include <cstdint>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Identifies the independently changing inputs that make a rendered artifact fresh.
     *
     * Scene/model, parameter, camera/view, viewport, and quality generations intentionally evolve
     * independently. This lets camera-only interaction reject stale view frames without forcing
     * expensive model-side work such as bounding-box or SDF recomputation.
     */
    struct RenderStamp
    {
        uint64_t sceneEpoch{0};     ///< Structural model/program generation.
        uint64_t parameterEpoch{0}; ///< Numeric parameter payload generation.
        uint64_t viewEpoch{0};      ///< Camera/view generation.
        uint64_t viewportEpoch{0};  ///< Render target size/generation.
        uint64_t qualityEpoch{0};   ///< Render settings/quality generation.
    };

    /**
     * @brief Selects which RenderStamp axes are relevant for a freshness comparison.
     */
    struct RenderStampMask
    {
        bool scene{true};
        bool parameters{true};
        bool view{true};
        bool viewport{true};
        bool quality{true};

        [[nodiscard]] static constexpr RenderStampMask all() noexcept
        {
            return {};
        }

        [[nodiscard]] static constexpr RenderStampMask sceneOnly() noexcept
        {
            return {.scene = true,
                    .parameters = false,
                    .view = false,
                    .viewport = false,
                    .quality = false};
        }

        [[nodiscard]] static constexpr RenderStampMask sceneAndParameters() noexcept
        {
            return {.scene = true,
                    .parameters = true,
                    .view = false,
                    .viewport = false,
                    .quality = false};
        }

        [[nodiscard]] static constexpr RenderStampMask displayFrame() noexcept
        {
            return {.scene = true,
                    .parameters = true,
                    .view = true,
                    .viewport = true,
                    .quality = true};
        }

        [[nodiscard]] static constexpr RenderStampMask interactiveFrame() noexcept
        {
            return {.scene = true,
                    .parameters = true,
                    .view = true,
                    .viewport = true,
                    .quality = false};
        }

        [[nodiscard]] static constexpr RenderStampMask heavyGeometryTask() noexcept
        {
            return sceneAndParameters();
        }
    };

    [[nodiscard]] constexpr bool matches(RenderStamp const & candidate,
                                         RenderStamp const & required,
                                         RenderStampMask const mask = RenderStampMask::all()) noexcept
    {
        return (!mask.scene || candidate.sceneEpoch == required.sceneEpoch) &&
               (!mask.parameters || candidate.parameterEpoch == required.parameterEpoch) &&
               (!mask.view || candidate.viewEpoch == required.viewEpoch) &&
               (!mask.viewport || candidate.viewportEpoch == required.viewportEpoch) &&
               (!mask.quality || candidate.qualityEpoch == required.qualityEpoch);
    }

    [[nodiscard]] constexpr bool isOlderThan(RenderStamp const & candidate,
                                             RenderStamp const & required,
                                             RenderStampMask const mask = RenderStampMask::all()) noexcept
    {
        return (mask.scene && candidate.sceneEpoch < required.sceneEpoch) ||
               (mask.parameters && candidate.parameterEpoch < required.parameterEpoch) ||
               (mask.view && candidate.viewEpoch < required.viewEpoch) ||
               (mask.viewport && candidate.viewportEpoch < required.viewportEpoch) ||
               (mask.quality && candidate.qualityEpoch < required.qualityEpoch);
    }

    /**
     * @brief Testable semantic task kinds requested by the render-update coordinator.
     */
    enum class RenderTaskType
    {
        RealtimeFullFrame,
        ProgressiveHighQualityChunk,
        LowResolutionPreview,
        StreamingPreview,
        BoundingBoxUpdate,
        SdfPrecomputation,
        ParameterUpload,
        ProgramCompilation
    };

    enum class RenderTaskStatus
    {
        Completed,
        Cancelled,
        Failed
    };

    /**
     * @brief Pure description of render/update work before it is translated to GPU/UI calls.
     */
    struct RenderTaskRequest
    {
        uint64_t requestId{0};
        RenderTaskType type{RenderTaskType::ProgressiveHighQualityChunk};
        RenderStamp stamp{};
        uint32_t width{0};
        uint32_t height{0};
        size_t startLine{0};
        size_t lineCount{0};
    };

    /**
     * @brief Pure completion metadata returned to the render-update coordinator.
     */
    struct RenderTaskResult
    {
        uint64_t requestId{0};
        RenderTaskType type{RenderTaskType::ProgressiveHighQualityChunk};
        RenderStamp stamp{};
        RenderTaskStatus status{RenderTaskStatus::Completed};
        uint64_t durationNs{0};
        bool producedDisplayFrame{false};
        bool completedFrame{false};

        [[nodiscard]] constexpr bool succeeded() const noexcept
        {
            return status == RenderTaskStatus::Completed;
        }

        [[nodiscard]] constexpr bool isCurrentFor(RenderStamp const & required,
                                                  RenderStampMask const mask) const noexcept
        {
            return succeeded() && matches(stamp, required, mask);
        }

        [[nodiscard]] constexpr bool isDisplayableFor(RenderStamp const & required,
                                                      RenderStampMask const mask = RenderStampMask::displayFrame()) const noexcept
        {
            return producedDisplayFrame && isCurrentFor(required, mask);
        }
    };
}
