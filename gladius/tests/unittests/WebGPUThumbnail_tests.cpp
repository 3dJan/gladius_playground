#include "Document.h"
#include "EventLogger.h"
#include "compute/ApplicationComputeRuntime.h"

#include <gtest/gtest.h>
#include <lodepng.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

namespace gladius::tests
{
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

                auto const filePath =
                    std::filesystem::path("testdata") / "honeycombecase_connectable_007.3mf";
        ASSERT_TRUE(std::filesystem::exists(filePath)) << filePath;
        ASSERT_NO_THROW(document.load(filePath));

        constexpr unsigned THUMBNAIL_SIZE = 64u;
        PlainImage image;
        ASSERT_NO_THROW(image = document.createThumbnailPng(THUMBNAIL_SIZE));
        ASSERT_FALSE(image.data.empty());
        EXPECT_EQ(image.width, THUMBNAIL_SIZE);
        EXPECT_EQ(image.height, THUMBNAIL_SIZE);

        std::vector<unsigned char> decoded;
        unsigned decodedWidth = 0u;
        unsigned decodedHeight = 0u;
        auto const error = lodepng::decode(decoded, decodedWidth, decodedHeight, image.data);
        ASSERT_EQ(error, 0u) << lodepng_error_text(error);
        EXPECT_EQ(decodedWidth, THUMBNAIL_SIZE);
        EXPECT_EQ(decodedHeight, THUMBNAIL_SIZE);
        ASSERT_EQ(decoded.size(), static_cast<std::size_t>(THUMBNAIL_SIZE) * THUMBNAIL_SIZE * 4u);

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
