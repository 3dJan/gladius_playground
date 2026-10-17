#include "Document.h"
#include "EventLogger.h"
#include "compute/ApplicationComputeRuntime.h"
#include "ui/ThreemfThumbnailExtractor.h"

#include <gtest/gtest.h>
#include <lodepng.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace gladius::tests
{
        TEST(ThreemfThumbnailExtractor, BufferInput_ExtractsPackageThumbnail)
        {
                auto const filePath = std::filesystem::path("testdata") / "SimpleGyroid.3mf";
                std::ifstream archiveFile(filePath, std::ios::binary);
                ASSERT_TRUE(archiveFile) << filePath;

                std::vector<unsigned char> archiveData(
                    (std::istreambuf_iterator<char>(archiveFile)), std::istreambuf_iterator<char>());
                ASSERT_FALSE(archiveData.empty());

                auto const fileResult = ui::ThreemfThumbnailExtractor::extractThumbnailDataOnly(filePath);
                auto const bufferResult =
                    ui::ThreemfThumbnailExtractor::extractThumbnailDataOnlyFromBuffer(archiveData);

                ASSERT_TRUE(fileResult.success) << fileResult.errorMessage;
                ASSERT_TRUE(bufferResult.success) << bufferResult.errorMessage;
                EXPECT_EQ(bufferResult.fileSize, archiveData.size());
                EXPECT_EQ(bufferResult.width, fileResult.width);
                EXPECT_EQ(bufferResult.height, fileResult.height);
                EXPECT_EQ(bufferResult.decodedPixels, fileResult.decodedPixels);
        }

#if defined(GLADIUS_ENABLE_WEBGPU)
    TEST(WebGPUThumbnail, CorelessDocument_CreateThumbnailPng_ReturnsDecodableImage)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
        if (!runtime->isAvailable())
        {
            GTEST_SKIP() << "WebGPU runtime unavailable: " << runtime->getErrorMessage();
        }

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document(logger);
        document.setBackendRuntime(runtime.get());
        auto const backend = document.getBackendKind();
        ASSERT_TRUE(backend.has_value());
        EXPECT_EQ(*backend, compute::ComputeBackendKind::WebGPU);

        auto const filePath =
          std::filesystem::path("testdata") / "honeycombecase_connectable_007.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        constexpr unsigned THUMBNAIL_WIDTH = 64u;
        constexpr unsigned THUMBNAIL_HEIGHT = 48u;
        PlainImage image;
        ASSERT_NO_THROW(image = document.createThumbnailPng(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT));
        ASSERT_FALSE(image.data.empty());
        EXPECT_EQ(image.width, THUMBNAIL_WIDTH);
        EXPECT_EQ(image.height, THUMBNAIL_HEIGHT);

        std::vector<unsigned char> decoded;
        unsigned decodedWidth = 0u;
        unsigned decodedHeight = 0u;
        auto const error = lodepng::decode(decoded, decodedWidth, decodedHeight, image.data);
        ASSERT_EQ(error, 0u) << lodepng_error_text(error);
        EXPECT_EQ(decodedWidth, THUMBNAIL_WIDTH);
        EXPECT_EQ(decodedHeight, THUMBNAIL_HEIGHT);
        ASSERT_EQ(decoded.size(),
                  static_cast<std::size_t>(THUMBNAIL_WIDTH) * THUMBNAIL_HEIGHT * 4u);

        bool hasPixelVariation = false;
        for (std::size_t offset = 4u; offset < decoded.size(); offset += 4u)
        {
            if (!std::equal(decoded.begin(), decoded.begin() + 4, decoded.begin() + offset))
            {
                hasPixelVariation = true;
                break;
            }
        }
        EXPECT_TRUE(hasPixelVariation);
    }
#endif
}
