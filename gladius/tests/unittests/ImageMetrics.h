#pragma once

/// @file ImageMetrics.h
/// @brief Perceptual image comparison metrics for rendered RGBA8 frames.
///
/// Operates on the pixel layout of compute::FrameResult::pixels: one
/// std::uint32_t per pixel, packed little-endian as 0xAABBGGRR.

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace qa
{
    /// @brief A rendered frame: dimensions and one packed RGBA8 value per pixel.
    struct Image
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint32_t> pixels;

        [[nodiscard]] std::size_t pixelCount() const noexcept
        {
            return pixels.size();
        }
    };

    /// @brief Extract one 8-bit channel (0=R, 1=G, 2=B, 3=A) of a packed pixel.
    [[nodiscard]] inline std::uint8_t channelOf(std::uint32_t pixel, int channel) noexcept
    {
        return static_cast<std::uint8_t>((pixel >> (8 * channel)) & 0xFFu);
    }

    /**
     * @brief Per-channel PSNR in dB between two same-sized images.
     * @return PSNR for {R,G,B}; +infinity for identical images, NaN on size mismatch.
     */
    [[nodiscard]] inline std::array<double, 3> computePsnr(Image const & a, Image const & b)
    {
        auto const nan = std::numeric_limits<double>::quiet_NaN();
        if (a.pixelCount() != b.pixelCount())
        {
            return {nan, nan, nan};
        }
        if (a.pixels == b.pixels)
        {
            auto const inf = std::numeric_limits<double>::infinity();
            return {inf, inf, inf};
        }

        std::array<double, 3> mse{};
        for (std::size_t i = 0; i < a.pixels.size(); ++i)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                double const d = static_cast<double>(channelOf(a.pixels[i], channel)) - static_cast<double>(channelOf(b.pixels[i], channel));
                mse[static_cast<std::size_t>(channel)] += d * d;
            }
        }
        double const inv = 1.0 / static_cast<double>(a.pixels.size());
        std::array<double, 3> psnr{};
        for (int channel = 0; channel < 3; ++channel)
        {
            double & value = mse[static_cast<std::size_t>(channel)];
            value *= inv;
            psnr[static_cast<std::size_t>(channel)] = (value <= 0.0) ? std::numeric_limits<double>::infinity()
                                                                     : 10.0 * std::log10(255.0 * 255.0 / value);
        }
        return psnr;
    }

    /// @brief Scalar PSNR: the worst (minimum) of the RGB channels.
    [[nodiscard]] inline double computePsnrRgb(Image const & a, Image const & b)
    {
        auto const psnr = computePsnr(a, b);
        return std::min({psnr[0], psnr[1], psnr[2]});
    }

    /// @brief Largest absolute per-channel difference over all pixels and channels; 255 on size mismatch.
    [[nodiscard]] inline std::uint8_t computeMaxAbsChannelDiff(Image const & a, Image const & b)
    {
        if (a.pixelCount() != b.pixelCount())
        {
            return 255;
        }
        std::uint8_t maxDiff{};
        for (std::size_t i = 0; i < a.pixels.size(); ++i)
        {
            for (int channel = 0; channel < 4; ++channel)
            {
                int const d = std::abs(static_cast<int>(channelOf(a.pixels[i], channel)) - static_cast<int>(channelOf(b.pixels[i], channel)));
                maxDiff = static_cast<std::uint8_t>(std::max<int>(maxDiff, d));
            }
        }
        return maxDiff;
    }

    /// @brief Mean absolute per-channel difference over all pixels and RGB channels; NaN on size mismatch.
    [[nodiscard]] inline double computeMeanAbsChannelDiff(Image const & a, Image const & b)
    {
        if (a.pixelCount() != b.pixelCount())
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (a.pixels.empty())
        {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < a.pixels.size(); ++i)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                sum += std::abs(static_cast<int>(channelOf(a.pixels[i], channel)) - static_cast<int>(channelOf(b.pixels[i], channel)));
            }
        }
        return sum / (3.0 * static_cast<double>(a.pixels.size()));
    }

    /**
     * @brief Mean absolute per-channel difference restricted to foreground pixels.
     * @param mask One byte per pixel; non-zero selects foreground.
     */
    [[nodiscard]] inline double computeMeanAbsChannelDiffMasked(Image const & a, Image const & b, std::vector<std::uint8_t> const & mask)
    {
        if (a.pixelCount() != b.pixelCount() || mask.size() != a.pixelCount())
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        double sum = 0.0;
        std::size_t count{};
        for (std::size_t i = 0; i < a.pixels.size(); ++i)
        {
            if (mask[i] == 0)
            {
                continue;
            }
            ++count;
            for (int channel = 0; channel < 3; ++channel)
            {
                sum += std::abs(static_cast<int>(channelOf(a.pixels[i], channel)) - static_cast<int>(channelOf(b.pixels[i], channel)));
            }
        }
        return (count == 0) ? 0.0 : sum / (3.0 * static_cast<double>(count));
    }

    /// @brief Foreground mask: pixels differing from backgroundRgb by more than tolerance in any channel.
    [[nodiscard]] inline std::vector<std::uint8_t> computeForegroundMask(Image const & image, std::uint32_t backgroundRgb, int tolerance = 8)
    {
        std::vector<std::uint8_t> mask(image.pixels.size(), 0);
        for (std::size_t i = 0; i < image.pixels.size(); ++i)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                if (std::abs(static_cast<int>(channelOf(image.pixels[i], channel)) - static_cast<int>(channelOf(backgroundRgb, channel))) > tolerance)
                {
                    mask[i] = 1;
                    break;
                }
            }
        }
        return mask;
    }

    /// @brief Intersection-over-union of two foreground masks; 1.0 when both are empty, NaN on size mismatch.
    [[nodiscard]] inline double computeMaskIou(std::vector<std::uint8_t> const & a, std::vector<std::uint8_t> const & b)
    {
        if (a.size() != b.size())
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        std::size_t intersection{};
        std::size_t unionCount{};
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            bool const inA = a[i] != 0;
            bool const inB = b[i] != 0;
            intersection += (inA && inB) ? 1u : 0u;
            unionCount += (inA || inB) ? 1u : 0u;
        }
        return (unionCount == 0) ? 1.0 : static_cast<double>(intersection) / static_cast<double>(unionCount);
    }

    namespace detail
    {
        /// @brief Rec. 601 luminance plane, the input of the SSIM computation.
        [[nodiscard]] inline std::vector<double> toLuminance(Image const & image)
        {
            std::vector<double> luminance(image.pixels.size());
            for (std::size_t i = 0; i < image.pixels.size(); ++i)
            {
                luminance[i] = 0.299 * channelOf(image.pixels[i], 0) + 0.587 * channelOf(image.pixels[i], 1) + 0.114 * channelOf(image.pixels[i], 2);
            }
            return luminance;
        }
    }

    /**
     * @brief Standard SSIM (Wang et al. 2004) on luminance, 11x11 Gaussian window, sigma 1.5.
     * @return Mean SSIM over all fully-valid window positions, in [0,1]; NaN on size mismatch.
     */
    [[nodiscard]] inline double computeSsim(Image const & a, Image const & b)
    {
        if (a.width != b.width || a.height != b.height || a.pixelCount() != b.pixelCount())
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (a.pixels == b.pixels)
        {
            return 1.0;
        }

        constexpr int RADIUS = 5; // 11x11 window
        constexpr double SIGMA = 1.5;
        constexpr double DYNAMIC_RANGE = 255.0;
        constexpr double C1 = (0.01 * DYNAMIC_RANGE) * (0.01 * DYNAMIC_RANGE);
        constexpr double C2 = (0.03 * DYNAMIC_RANGE) * (0.03 * DYNAMIC_RANGE);

        std::array<double, 2 * RADIUS + 1> kernel{};
        double kernelSum = 0.0;
        for (int offset = -RADIUS; offset <= RADIUS; ++offset)
        {
            double const weight = std::exp(-(static_cast<double>(offset) * offset) / (2.0 * SIGMA * SIGMA));
            kernel[static_cast<std::size_t>(offset + RADIUS)] = weight;
            kernelSum += weight;
        }
        for (double & weight : kernel)
        {
            weight /= kernelSum;
        }

        std::vector<double> const luminanceA = detail::toLuminance(a);
        std::vector<double> const luminanceB = detail::toLuminance(b);
        auto const width = static_cast<int>(a.width);
        auto const height = static_cast<int>(a.height);

        double ssimSum = 0.0;
        std::size_t windowCount{};
        for (int y = RADIUS; y < height - RADIUS; ++y)
        {
            for (int x = RADIUS; x < width - RADIUS; ++x)
            {
                double meanA = 0.0;
                double meanB = 0.0;
                for (int dy = -RADIUS; dy <= RADIUS; ++dy)
                {
                    for (int dx = -RADIUS; dx <= RADIUS; ++dx)
                    {
                        double const w = kernel[static_cast<std::size_t>(dy + RADIUS)] * kernel[static_cast<std::size_t>(dx + RADIUS)];
                        std::size_t const index = static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x + dx);
                        meanA += w * luminanceA[index];
                        meanB += w * luminanceB[index];
                    }
                }
                double varA = 0.0;
                double varB = 0.0;
                double covAB = 0.0;
                for (int dy = -RADIUS; dy <= RADIUS; ++dy)
                {
                    for (int dx = -RADIUS; dx <= RADIUS; ++dx)
                    {
                        double const w = kernel[static_cast<std::size_t>(dy + RADIUS)] * kernel[static_cast<std::size_t>(dx + RADIUS)];
                        std::size_t const index = static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x + dx);
                        double const deltaA = luminanceA[index] - meanA;
                        double const deltaB = luminanceB[index] - meanB;
                        varA += w * deltaA * deltaA;
                        varB += w * deltaB * deltaB;
                        covAB += w * deltaA * deltaB;
                    }
                }
                double const numerator = (2.0 * meanA * meanB + C1) * (2.0 * covAB + C2);
                double const denominator = (meanA * meanA + meanB * meanB + C1) * (varA + varB + C2);
                ssimSum += numerator / denominator;
                ++windowCount;
            }
        }
        return (windowCount == 0) ? 1.0 : ssimSum / static_cast<double>(windowCount);
    }

    /// @brief Overlap of normalized RGB color histograms (32 bins per channel, foreground only), in [0,1].
    [[nodiscard]] inline double computeColorHistogramOverlap(Image const & a, Image const & b, std::vector<std::uint8_t> const & mask)
    {
        if (a.pixelCount() != b.pixelCount() || mask.size() != a.pixelCount())
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        constexpr int BINS = 32;
        constexpr std::size_t BIN_COUNT = static_cast<std::size_t>(BINS) * BINS * BINS;
        std::vector<std::size_t> histogramA(BIN_COUNT, 0);
        std::vector<std::size_t> histogramB(BIN_COUNT, 0);
        auto const binIndexOf = [](Image const & image, std::size_t pixelIndex)
        {
            auto const bin = [](std::uint8_t value) { return static_cast<std::size_t>(value) * BINS / 256; };
            return bin(channelOf(image.pixels[pixelIndex], 0)) * BINS * BINS + bin(channelOf(image.pixels[pixelIndex], 1)) * BINS + bin(channelOf(image.pixels[pixelIndex], 2));
        };
        std::size_t totalA{};
        std::size_t totalB{};
        for (std::size_t i = 0; i < a.pixels.size(); ++i)
        {
            if (mask[i] == 0)
            {
                continue;
            }
            ++histogramA[binIndexOf(a, i)];
            ++histogramB[binIndexOf(b, i)];
            ++totalA;
            ++totalB;
        }
        if (totalA == 0 && totalB == 0)
        {
            return 1.0;
        }
        if (totalA == 0 || totalB == 0)
        {
            return 0.0;
        }
        double overlap = 0.0;
        for (std::size_t bin = 0; bin < BIN_COUNT; ++bin)
        {
            overlap += std::min(static_cast<double>(histogramA[bin]) / static_cast<double>(totalA),
                                static_cast<double>(histogramB[bin]) / static_cast<double>(totalB));
        }
        return overlap;
    }

    /// @brief Count pixels equal to the shader's invalid-sample debug color (magenta 255,0,255).
    [[nodiscard]] inline std::size_t countInvalidPixels(Image const & image)
    {
        std::size_t count{};
        for (std::uint32_t const pixel : image.pixels)
        {
            if (channelOf(pixel, 0) == 255 && channelOf(pixel, 1) == 0 && channelOf(pixel, 2) == 255)
            {
                ++count;
            }
        }
        return count;
    }
}
