#include "ImageMetrics.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace qa::tests
{
    namespace
    {
        /// @brief Build a solid-color image.
        [[nodiscard]] Image makeImage(std::uint32_t width, std::uint32_t height, std::uint32_t packedRgba)
        {
            Image image;
            image.width = width;
            image.height = height;
            image.pixels.assign(static_cast<std::size_t>(width) * height, packedRgba);
            return image;
        }

        /// @brief Pack RGBA components into the FrameResult pixel layout (0xAABBGGRR).
        [[nodiscard]] constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
        {
            return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
        }
    }

    TEST(ImageMetrics, ChannelOf_PackedPixel_ExtractsRgbaBytes)
    {
        std::uint32_t const pixel = rgba(0x12, 0x34, 0x56, 0x78);
        EXPECT_EQ(channelOf(pixel, 0), 0x12);
        EXPECT_EQ(channelOf(pixel, 1), 0x34);
        EXPECT_EQ(channelOf(pixel, 2), 0x56);
        EXPECT_EQ(channelOf(pixel, 3), 0x78);
    }

    TEST(ImageMetrics, Psnr_IdenticalImages_ReturnsInfinity)
    {
        Image const a = makeImage(8, 8, rgba(10, 20, 30));
        Image const b = makeImage(8, 8, rgba(10, 20, 30));
        auto const psnr = computePsnr(a, b);
        for (double const value : psnr)
        {
            EXPECT_TRUE(std::isinf(value));
        }
        EXPECT_TRUE(std::isinf(computePsnrRgb(a, b)));
    }

    TEST(ImageMetrics, Psnr_UniformOffsetOfEight_ReturnsKnownDecibels)
    {
        // MSE = 8^2 = 64 per channel -> PSNR = 10*log10(255^2/64) = 28.13 dB.
        Image const a = makeImage(8, 8, rgba(100, 100, 100));
        Image const b = makeImage(8, 8, rgba(108, 108, 108));
        auto const psnr = computePsnr(a, b);
        double const expected = 10.0 * std::log10(255.0 * 255.0 / 64.0);
        for (double const value : psnr)
        {
            EXPECT_NEAR(value, expected, 1e-9);
        }
    }

    TEST(ImageMetrics, Psnr_DifferentSizes_ReturnsNaN)
    {
        Image const a = makeImage(8, 8, 0);
        Image const b = makeImage(4, 4, 0);
        auto const psnr = computePsnr(a, b);
        for (double const value : psnr)
        {
            EXPECT_TRUE(std::isnan(value));
        }
    }

    TEST(ImageMetrics, Ssim_IdenticalImages_ReturnsOne)
    {
        Image const a = makeImage(32, 32, rgba(7, 9, 11));
        EXPECT_DOUBLE_EQ(computeSsim(a, a), 1.0);
    }

    TEST(ImageMetrics, Ssim_UniformImagesDifferingByConstant_NearOne)
    {
        // SSIM is contrast/structure based. For flat images the variance terms vanish,
        // leaving the luminance term (2*m1*m2+C1)/(m1^2+m2^2+C1) = 0.99548 for means 100 vs 110.
        Image const a = makeImage(32, 32, rgba(100, 100, 100));
        Image const b = makeImage(32, 32, rgba(110, 110, 110));
        EXPECT_NEAR(computeSsim(a, b), 0.99548, 1e-4);
    }

    TEST(ImageMetrics, Ssim_InvertedCheckerboard_DropsWellBelowOne)
    {
        auto makeChecker = [](std::uint32_t const dark, std::uint32_t const light)
        {
            Image image = makeImage(32, 32, dark);
            for (std::uint32_t y = 0; y < 32; ++y)
            {
                for (std::uint32_t x = 0; x < 32; ++x)
                {
                    if (((x / 4) + (y / 4)) % 2 == 0)
                    {
                        image.pixels[static_cast<std::size_t>(y) * 32 + x] = light;
                    }
                }
            }
            return image;
        };
        double const ssim = computeSsim(makeChecker(rgba(0, 0, 0), rgba(255, 255, 255)), makeChecker(rgba(255, 255, 255), rgba(0, 0, 0)));
        EXPECT_LT(ssim, 0.5);
        EXPECT_GE(ssim, -1.0);
    }

    TEST(ImageMetrics, Ssim_DifferentSizes_ReturnsNaN)
    {
        Image const a = makeImage(16, 16, 0);
        Image const b = makeImage(8, 8, 0);
        EXPECT_TRUE(std::isnan(computeSsim(a, b)));
    }

    TEST(ImageMetrics, ChannelDiff_KnownOffsets_ReturnsMaxAndMean)
    {
        Image const a = makeImage(4, 4, rgba(100, 100, 100));
        Image b = a;
        b.pixels[0] = rgba(150, 100, 100); // one pixel differs by 50 in R
        EXPECT_EQ(computeMaxAbsChannelDiff(a, b), 50);
        EXPECT_NEAR(computeMeanAbsChannelDiff(a, b), 50.0 / (3.0 * 16.0), 1e-12);
    }

    TEST(ImageMetrics, ForegroundMask_TwoToneImage_SeparatesModelFromBackground)
    {
        Image image = makeImage(4, 4, rgba(0, 0, 0));
        image.pixels[0] = rgba(200, 100, 50);
        image.pixels[5] = rgba(180, 90, 40);
        auto const mask = computeForegroundMask(image, rgba(0, 0, 0));
        EXPECT_EQ(mask.size(), 16u);
        EXPECT_EQ(mask[0], 1);
        EXPECT_EQ(mask[5], 1);
        EXPECT_EQ(mask[1], 0);
        EXPECT_EQ(mask[15], 0);
    }

    TEST(ImageMetrics, MaskIou_HalfOverlappingMasks_ReturnsExpectedRatio)
    {
        std::vector<std::uint8_t> const a{1, 1, 1, 0};
        std::vector<std::uint8_t> const b{0, 1, 1, 1};
        EXPECT_NEAR(computeMaskIou(a, b), 2.0 / 4.0, 1e-12);
    }

    TEST(ImageMetrics, MaskIou_BothEmpty_ReturnsOne)
    {
        std::vector<std::uint8_t> const a(8, 0);
        std::vector<std::uint8_t> const b(8, 0);
        EXPECT_DOUBLE_EQ(computeMaskIou(a, b), 1.0);
    }

    TEST(ImageMetrics, ColorHistogramOverlap_IdenticalForeground_ReturnsOne)
    {
        Image const a = makeImage(8, 8, rgba(10, 200, 30));
        Image const b = makeImage(8, 8, rgba(10, 200, 30));
        auto const mask = computeForegroundMask(a, rgba(0, 0, 0));
        EXPECT_NEAR(computeColorHistogramOverlap(a, b, mask), 1.0, 1e-12);
    }

    TEST(ImageMetrics, ColorHistogramOverlap_DisjointColors_ReturnsZero)
    {
        Image const a = makeImage(8, 8, rgba(0, 0, 0));
        Image const b = makeImage(8, 8, rgba(255, 255, 255));
        // Mask built from a union-like selection: use a background that marks all pixels foreground.
        auto const mask = computeForegroundMask(a, rgba(128, 128, 128), 100);
        EXPECT_EQ(mask.size(), 64u);
        EXPECT_NEAR(computeColorHistogramOverlap(a, b, mask), 0.0, 1e-12);
    }

    TEST(ImageMetrics, CountInvalidPixels_MagentaDebugPixels_Counted)
    {
        Image image = makeImage(4, 4, rgba(1, 2, 3));
        image.pixels[3] = rgba(255, 0, 255);
        image.pixels[9] = rgba(255, 0, 255);
        EXPECT_EQ(countInvalidPixels(image), 2u);
    }

    TEST(ImageMetrics, Ssim_GaussianNoiseOnFlatField_IsNoiseSensitive)
    {
        // Calibration for the HQ gate: sigma~5 per-channel noise on a flat field gives
        // PSNR ~34 dB and SSIM ~0.845. The HQ gate (PSNR >= 40 dB, SSIM >= 0.98) is
        // therefore substantially tighter than visible noise, which is intended.
        Image noisy = makeImage(64, 64, rgba(128, 128, 128));
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 5.0);
        for (auto & pixel : noisy.pixels)
        {
            auto const clamp = [&noise, &rng]()
            {
                return static_cast<std::uint8_t>(std::min(255, std::max(0, static_cast<int>(128.0 + noise(rng)))));
            };
            pixel = rgba(clamp(), clamp(), clamp());
        }
        Image const reference = makeImage(64, 64, rgba(128, 128, 128));
        EXPECT_GT(computeSsim(reference, noisy), 0.5);
        EXPECT_LT(computeSsim(reference, noisy), 1.0);
        EXPECT_LT(computePsnrRgb(reference, noisy), 40.0);
    }

    TEST(ImageMetrics, SizeMismatch_ReturnsFailingSentinels)
    {
        // Every metric must return a value that fails its >= or <= gate on mismatch,
        // so a mis-wired reference cannot silently pass.
        Image const a = makeImage(8, 8, 0);
        Image const small = makeImage(4, 4, 0);
        EXPECT_TRUE(std::isnan(computeMeanAbsChannelDiff(a, small)));
        EXPECT_EQ(computeMaxAbsChannelDiff(a, small), 255);
        EXPECT_TRUE(std::isnan(computeMaskIou(std::vector<std::uint8_t>(4, 1), std::vector<std::uint8_t>(8, 1))));
        EXPECT_TRUE(std::isnan(computeMeanAbsChannelDiffMasked(a, small, std::vector<std::uint8_t>(64, 1))));
    }

    TEST(ImageMetrics, MaskedChannelDiff_MaskExcludesDifferingPixel_IgnoresIt)
    {
        Image const a = makeImage(2, 2, rgba(100, 100, 100));
        Image b = a;
        b.pixels[0] = rgba(0, 0, 0); // differs, but is masked out
        std::vector<std::uint8_t> const mask{0, 1, 1, 1};
        EXPECT_DOUBLE_EQ(computeMeanAbsChannelDiffMasked(a, b, mask), 0.0);
    }
}
