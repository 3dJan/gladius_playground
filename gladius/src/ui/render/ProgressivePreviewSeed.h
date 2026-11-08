#pragma once

#include "FramePresentationTypes.h"
#include "compute/RenderContracts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace gladius::ui::async_rendering
{
    inline constexpr std::size_t MAX_PROGRESSIVE_PREVIEW_SEED_PIXELS = 10u * 1024u * 1024u;

    [[nodiscard]] constexpr bool isPreviewFrameSource(FramePresentationSource const source) noexcept
    {
        return source == FramePresentationSource::LowResolutionPreview ||
               source == FramePresentationSource::StreamingPreview;
    }

    [[nodiscard]] inline bool isCompleteFrame(compute::RenderFrame const & frame) noexcept
    {
        return frame.isValid() && frame.firstRow == 0u && frame.endRow == frame.height;
    }

    [[nodiscard]] inline bool frameMatchesCandidate(compute::RenderFrame const & frame,
                                                    RenderStamp const & stamp) noexcept
    {
        return frame.freshness.sceneGeneration == stamp.sceneEpoch &&
               frame.freshness.viewGeneration == stamp.viewEpoch &&
               frame.freshness.parameterGeneration == stamp.parameterEpoch;
    }

    [[nodiscard]] inline bool isCompletePreviewSeedFrame(
      compute::RenderFrame const & frame,
      FramePresentationCandidate const & candidate) noexcept
    {
        return isCompleteFrame(frame) && frame.pixels.size() <= MAX_PROGRESSIVE_PREVIEW_SEED_PIXELS &&
               candidate.completedFrame && candidate.quality == FramePresentationQuality::Preview &&
               isPreviewFrameSource(candidate.source) && frameMatchesCandidate(frame, candidate.stamp);
    }

    /**
     * @brief Requires a fresh preview for every render-state or viewport change.
     *
     * The preview may have a lower resolution than the HQ target, but must have been rendered
     * for the same scene, parameters, camera, viewport, and rendering settings.
     */
    [[nodiscard]] inline constexpr bool matchesProgressivePreviewSeedStamp(
      RenderStamp const & previewStamp,
      RenderStamp const & progressiveStamp) noexcept
    {
        return matches(previewStamp, progressiveStamp, RenderStampMask::displayFrame());
    }

    /// Metadata for the result image currently available as a low-resolution preview seed.
    struct ProgressivePreviewSeedState
    {
        bool hasPreview{false};
        std::uint64_t epoch{0};
        std::uint64_t viewEpoch{0};
        std::uint64_t paramGeneration{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
        RenderStamp stamp{};
    };

    /// Metadata for the progressive HQ buffer that would be seeded from the result image.
    struct ProgressivePreviewSeedTarget
    {
        std::uint64_t epoch{0};
        std::uint64_t viewEpoch{0};
        std::uint64_t paramGeneration{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
        RenderStamp stamp{};
    };

    /// Rejects stale, incompatible, resized, or missing result-image seeds.
    [[nodiscard]] constexpr bool isCompatibleProgressivePreviewSeed(
      ProgressivePreviewSeedState const & preview,
      ProgressivePreviewSeedTarget const & progressive) noexcept
    {
        return preview.hasPreview && preview.epoch == progressive.epoch &&
               preview.viewEpoch == progressive.viewEpoch &&
               preview.paramGeneration == progressive.paramGeneration &&
               preview.width > 0u && preview.height > 0u &&
               preview.width == progressive.width && preview.height == progressive.height &&
               matchesProgressivePreviewSeedStamp(preview.stamp, progressive.stamp);
    }

    [[nodiscard]] [[nodiscard]] inline bool canSeedProgressiveFrame(
      compute::RenderFrame const & previewFrame,
      FramePresentationCandidate const & previewCandidate,
      compute::RenderFrame const & progressiveFrame,
      FramePresentationCandidate const & progressiveCandidate) noexcept
    {
        return isCompletePreviewSeedFrame(previewFrame, previewCandidate) &&
               progressiveFrame.isValid() && progressiveFrame.firstRow == 0u &&
               progressiveFrame.endRow < progressiveFrame.height &&
               frameMatchesCandidate(progressiveFrame, progressiveCandidate.stamp) &&
               progressiveCandidate.source == FramePresentationSource::ProgressiveHighQuality &&
               progressiveCandidate.quality == FramePresentationQuality::ProgressivePartial &&
               !progressiveCandidate.completedFrame &&
               matchesProgressivePreviewSeedStamp(previewCandidate.stamp,
                                                  progressiveCandidate.stamp);
    }

    /**
     * @brief Bilinearly resample a complete RGBA frame into a caller-owned pixel buffer.
     *
     * The destination stride is measured in RGBA pixels and may exceed the active width, which
     * lets texture upload buffers keep their required row padding without an extra full-frame copy.
     */
    [[nodiscard]] inline bool resampleCompleteFrameToBuffer(
      compute::RenderFrame const & source,
      std::uint32_t const targetWidth,
      std::uint32_t const targetHeight,
      std::uint32_t * const targetPixels,
      std::size_t const targetStridePixels)
    {
        if (!isCompleteFrame(source) || targetWidth == 0u || targetHeight == 0u ||
            targetPixels == nullptr || targetStridePixels < targetWidth ||
            targetStridePixels > std::numeric_limits<std::size_t>::max() / targetHeight ||
            source.pixels.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        {
            return false;
        }

        auto const targetPixelCount = targetStridePixels * targetHeight;
        if (targetPixelCount > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
        {
            return false;
        }

        if (source.width == targetWidth && source.height == targetHeight)
        {
            for (std::uint32_t row = 0u; row < targetHeight; ++row)
            {
                std::copy_n(source.pixels.data() + static_cast<std::size_t>(row) * source.width,
                            targetWidth,
                            targetPixels + static_cast<std::size_t>(row) * targetStridePixels);
            }
            return true;
        }

        struct AxisSample
        {
            std::size_t lower{};
            std::size_t upper{};
                        std::uint16_t blend{};
        };

        auto makeAxisSamples = [](std::uint32_t const sourceSize, std::uint32_t const targetSize)
        {
                        constexpr std::uint32_t blendScale = 256u;
            std::vector<AxisSample> samples(targetSize);
            for (std::uint32_t index = 0u; index < targetSize; ++index)
            {
                double const coordinate =
                  (static_cast<double>(index) + 0.5) * sourceSize / targetSize - 0.5;
                double const lowerCoordinate = std::floor(coordinate);
                                double blend = coordinate - lowerCoordinate;
                auto lower = lowerCoordinate <= 0.0
                               ? std::size_t{0}
                               : static_cast<std::size_t>(lowerCoordinate);
                if (lower >= static_cast<std::size_t>(sourceSize - 1u))
                {
                    lower = static_cast<std::size_t>(sourceSize - 1u);
                    blend = 0.0;
                }
                                if (lowerCoordinate < 0.0)
                                {
                                        blend = 0.0;
                                }
                                auto const fixedBlend = static_cast<std::uint16_t>(
                                    std::clamp(std::lround(blend * blendScale), 0l, static_cast<long>(blendScale)));
                samples[index] = AxisSample{.lower = lower,
                                            .upper = std::min(lower + 1u,
                                                              static_cast<std::size_t>(sourceSize - 1u)),
                                                                                        .blend = fixedBlend};
            }
            return samples;
        };

        auto const xSamples = makeAxisSamples(source.width, targetWidth);
        auto const ySamples = makeAxisSamples(source.height, targetHeight);
        auto const * sourceBytes = reinterpret_cast<unsigned char const *>(source.pixels.data());
        auto * targetBytes = reinterpret_cast<unsigned char *>(targetPixels);
                if (source.height > std::numeric_limits<std::size_t>::max() / targetWidth)
                {
                        return false;
                }
                auto const horizontalPixelCount = static_cast<std::size_t>(source.height) * targetWidth;
                if (horizontalPixelCount > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
                {
                        return false;
                }
                std::vector<std::uint32_t> horizontalPixels(horizontalPixelCount);
                auto * horizontalBytes = reinterpret_cast<unsigned char *>(horizontalPixels.data());
                constexpr std::uint32_t blendScale = 256u;
                constexpr std::uint32_t rounding = blendScale / 2u;

                for (std::uint32_t y = 0u; y < source.height; ++y)
        {
                        auto const sourceRowOffset = static_cast<std::size_t>(y) * source.width;
                        auto const horizontalRowOffset = static_cast<std::size_t>(y) * targetWidth;
            for (std::uint32_t x = 0u; x < targetWidth; ++x)
            {
                auto const & xSample = xSamples[x];
                                auto const sourceLeft = (sourceRowOffset + xSample.lower) * 4u;
                                auto const sourceRight = (sourceRowOffset + xSample.upper) * 4u;
                                auto const horizontalOffset = (horizontalRowOffset + x) * 4u;
                                auto const upperWeight = static_cast<std::uint32_t>(xSample.blend);
                                auto const lowerWeight = blendScale - upperWeight;

                for (std::size_t channel = 0u; channel < 4u; ++channel)
                {
                                        auto const value =
                                            (static_cast<std::uint32_t>(sourceBytes[sourceLeft + channel]) * lowerWeight +
                                             static_cast<std::uint32_t>(sourceBytes[sourceRight + channel]) * upperWeight +
                                             rounding) >>
                                            8u;
                                        horizontalBytes[horizontalOffset + channel] = static_cast<unsigned char>(value);
                                }
                        }
                }

                for (std::uint32_t y = 0u; y < targetHeight; ++y)
                {
                        auto const & ySample = ySamples[y];
                        auto const targetRowOffset = static_cast<std::size_t>(y) * targetStridePixels;
                        auto const horizontalTopOffset = ySample.lower * targetWidth;
                        auto const horizontalBottomOffset = ySample.upper * targetWidth;
                        auto const lowerWeight = blendScale - static_cast<std::uint32_t>(ySample.blend);
                        auto const upperWeight = static_cast<std::uint32_t>(ySample.blend);
                        for (std::uint32_t x = 0u; x < targetWidth; ++x)
                        {
                                auto const horizontalTop = (horizontalTopOffset + x) * 4u;
                                auto const horizontalBottom = (horizontalBottomOffset + x) * 4u;
                                auto const targetOffset = (targetRowOffset + x) * 4u;
                                for (std::size_t channel = 0u; channel < 4u; ++channel)
                                {
                                        auto const value =
                                            (static_cast<std::uint32_t>(horizontalBytes[horizontalTop + channel]) *
                                                 lowerWeight +
                                             static_cast<std::uint32_t>(horizontalBytes[horizontalBottom + channel]) *
                                                 upperWeight +
                                             rounding) >>
                                            8u;
                                        targetBytes[targetOffset + channel] = static_cast<unsigned char>(value);
                }
            }
        }
        return true;
    }
}