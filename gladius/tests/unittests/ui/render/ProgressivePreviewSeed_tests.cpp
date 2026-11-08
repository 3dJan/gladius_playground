#include "ui/render/ProgressivePreviewSeed.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        std::uint32_t makeRgbaPixel(std::uint8_t const red,
                                    std::uint8_t const green,
                                    std::uint8_t const blue,
                                    std::uint8_t const alpha)
        {
            std::array<std::uint8_t, 4> const channels{red, green, blue, alpha};
            std::uint32_t pixel{};
            std::memcpy(&pixel, channels.data(), sizeof(pixel));
            return pixel;
        }

        std::array<std::uint8_t, 4> getRgbaChannels(std::uint32_t const pixel)
        {
            std::array<std::uint8_t, 4> channels{};
            std::memcpy(channels.data(), &pixel, sizeof(pixel));
            return channels;
        }

        compute::RenderFreshnessStamp makeFreshness()
        {
            return {.sceneGeneration = 3u, .viewGeneration = 5u, .parameterGeneration = 7u};
        }

        FramePresentationCandidate makePreviewCandidate()
        {
            return {.frameId = 11u,
                    .stamp = {.sceneEpoch = 3u,
                              .parameterEpoch = 7u,
                              .viewEpoch = 5u,
                              .viewportEpoch = 13u,
                              .qualityEpoch = 17u},
                    .quality = FramePresentationQuality::Preview,
                    .source = FramePresentationSource::LowResolutionPreview,
                    .completedFrame = true};
        }

        FramePresentationCandidate makeProgressiveCandidate(RenderStamp const stamp)
        {
            return {.frameId = 12u,
                    .stamp = stamp,
                    .quality = FramePresentationQuality::ProgressivePartial,
                    .source = FramePresentationSource::ProgressiveHighQuality,
                    .completedFrame = false};
        }

        ProgressivePreviewSeedState makePreviewSeedState()
        {
            return {.hasPreview = true,
                    .epoch = 3u,
                    .viewEpoch = 5u,
                    .paramGeneration = 7u,
                    .width = 2u,
                    .height = 2u,
                    .stamp = makePreviewCandidate().stamp};
        }

        ProgressivePreviewSeedTarget makePreviewSeedTarget()
        {
            return {.epoch = 3u,
                    .viewEpoch = 5u,
                    .paramGeneration = 7u,
                    .width = 2u,
                    .height = 2u,
                    .stamp = makePreviewCandidate().stamp};
        }

        compute::RenderFrame makePreviewFrame()
        {
            return {.width = 2u,
                    .height = 2u,
                    .firstRow = 0u,
                    .endRow = 2u,
                    .freshness = makeFreshness(),
                    .pixels = {makeRgbaPixel(0u, 0u, 0u, 255u),
                               makeRgbaPixel(100u, 0u, 0u, 255u),
                               makeRgbaPixel(0u, 100u, 0u, 255u),
                               makeRgbaPixel(100u, 100u, 0u, 255u)}};
        }
    }

    TEST(ProgressivePreviewSeed, MatchingCompletePreviewAndHqStamp_AllowsSeeding)
    {
        auto const preview = makePreviewFrame();
        auto const previewCandidate = makePreviewCandidate();
        compute::RenderFrame const progressiveFrame{.width = 4u,
                                                    .height = 4u,
                                                    .firstRow = 0u,
                                                    .endRow = 2u,
                                                    .freshness = makeFreshness(),
                                                    .pixels = std::vector<std::uint32_t>(8u)};
        auto progressiveCandidate = makeProgressiveCandidate(previewCandidate.stamp);

        EXPECT_TRUE(isCompletePreviewSeedFrame(preview, previewCandidate));
        EXPECT_TRUE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));

        ++progressiveCandidate.stamp.qualityEpoch;
        EXPECT_FALSE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));
    }

    TEST(ProgressivePreviewSeed, ResultImageSeedRequiresCurrentPreviewAndMatchingRenderState)
    {
        auto const preview = makePreviewSeedState();
        auto const target = makePreviewSeedTarget();
        EXPECT_TRUE(isCompatibleProgressivePreviewSeed(preview, target));

        auto resizedTarget = target;
        ++resizedTarget.stamp.viewportEpoch;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, resizedTarget));

        auto incompatibleTarget = target;
        ++incompatibleTarget.paramGeneration;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        incompatibleTarget = target;
        ++incompatibleTarget.stamp.sceneEpoch;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        incompatibleTarget = target;
        ++incompatibleTarget.stamp.parameterEpoch;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        incompatibleTarget = target;
        ++incompatibleTarget.stamp.viewEpoch;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        incompatibleTarget = target;
        ++incompatibleTarget.stamp.qualityEpoch;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        incompatibleTarget = target;
        ++incompatibleTarget.width;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(preview, incompatibleTarget));

        auto missingPreview = preview;
        missingPreview.hasPreview = false;
        EXPECT_FALSE(isCompatibleProgressivePreviewSeed(missingPreview, target));
    }

    TEST(ProgressivePreviewSeed, ViewportResizeWithSameView_RequiresFreshPreview)
    {
        auto const preview = makePreviewFrame();
        auto previewCandidate = makePreviewCandidate();
        compute::RenderFrame const progressiveFrame{.width = 4u,
                                                    .height = 4u,
                                                    .firstRow = 0u,
                                                    .endRow = 2u,
                                                    .freshness = makeFreshness(),
                                                    .pixels = std::vector<std::uint32_t>(8u)};
        auto progressiveCandidate = makeProgressiveCandidate(previewCandidate.stamp);
        ++progressiveCandidate.stamp.viewportEpoch;

        EXPECT_FALSE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));

        previewCandidate.stamp = progressiveCandidate.stamp;
        EXPECT_TRUE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));

        ++progressiveCandidate.stamp.viewEpoch;
        EXPECT_FALSE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));
    }

    TEST(ProgressivePreviewSeed, StaleViewOrPartialPreview_RejectsSeeding)
    {
        auto preview = makePreviewFrame();
        auto const previewCandidate = makePreviewCandidate();
        compute::RenderFrame const progressiveFrame{.width = 4u,
                                                    .height = 4u,
                                                    .firstRow = 0u,
                                                    .endRow = 2u,
                                                    .freshness = makeFreshness(),
                                                    .pixels = std::vector<std::uint32_t>(8u)};
        auto progressiveCandidate = makeProgressiveCandidate(previewCandidate.stamp);
        ++progressiveCandidate.stamp.viewEpoch;
        EXPECT_FALSE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));

        progressiveCandidate = makeProgressiveCandidate(previewCandidate.stamp);
        preview.firstRow = 1u;
        preview.endRow = 2u;
        preview.pixels.resize(2u);
        EXPECT_FALSE(canSeedProgressiveFrame(
          preview, previewCandidate, progressiveFrame, progressiveCandidate));
    }

    TEST(ProgressivePreviewSeed, ResampleCompleteFrameToBuffer_BilinearlyUpscalesAndPreservesStride)
    {
        auto const preview = makePreviewFrame();
        constexpr std::uint32_t targetWidth = 3u;
        constexpr std::uint32_t targetHeight = 3u;
        constexpr std::size_t targetStride = 4u;
        std::vector<std::uint32_t> output(targetStride * targetHeight, 0xDEADBEEFu);

        ASSERT_TRUE(resampleCompleteFrameToBuffer(
          preview, targetWidth, targetHeight, output.data(), targetStride));

        EXPECT_EQ(getRgbaChannels(output[0]), (std::array<std::uint8_t, 4>{0u, 0u, 0u, 255u}));
        EXPECT_EQ(getRgbaChannels(output[5]), (std::array<std::uint8_t, 4>{50u, 50u, 0u, 255u}));
        EXPECT_EQ(getRgbaChannels(output[10]), (std::array<std::uint8_t, 4>{100u, 100u, 0u, 255u}));
        EXPECT_EQ(output[3], 0xDEADBEEFu);
        EXPECT_EQ(output[7], 0xDEADBEEFu);
        EXPECT_EQ(output[11], 0xDEADBEEFu);
    }

    TEST(ProgressivePreviewSeed, ResampleCompleteFrameToBuffer_WithPartialSource_ReturnsFalse)
    {
        auto preview = makePreviewFrame();
        preview.firstRow = 1u;
        preview.endRow = 2u;
        preview.pixels.resize(2u);
        std::vector<std::uint32_t> output(4u);

        EXPECT_FALSE(resampleCompleteFrameToBuffer(preview, 2u, 2u, output.data(), 2u));
    }
}