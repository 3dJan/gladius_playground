#if defined(GLADIUS_ENABLE_WEBGPU)

#include "WebGPUQualityReferences.h"

#include "gtest/gtest.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gladius::tests::quality
{
    namespace
    {
        struct CorpusEntry
        {
            std::string_view model;
            std::uint32_t resolution;
        };

        /// @brief Reference corpus: HQ settings at these resolutions are stored as PNGs.
        constexpr std::array CORPUS = {
          CorpusEntry{.model = "SimpleGyroid.3mf", .resolution = 256u},
          CorpusEntry{.model = "SimpleGyroid.3mf", .resolution = 512u},
          CorpusEntry{.model = "ImplicitGyroid.3mf", .resolution = 256u},
          CorpusEntry{.model = "ImplicitGyroid.3mf", .resolution = 512u},
          CorpusEntry{.model = "SphereInACage.3mf", .resolution = 256u},
          CorpusEntry{.model = "SphereInACage.3mf", .resolution = 512u},
          CorpusEntry{.model = "lampbase.3mf", .resolution = 256u},
          CorpusEntry{.model = "lampbase.3mf", .resolution = 512u},
          CorpusEntry{.model = "RadialRadiator.3mf", .resolution = 256u},
          CorpusEntry{.model = "RadialRadiator.3mf", .resolution = 512u},
          CorpusEntry{.model = "webcam_003.3mf", .resolution = 256u},
          CorpusEntry{.model = "webcam_003.3mf", .resolution = 512u}};

        /// @brief Background color the raymarch shader writes for empty space (vec3(0.1) -> 26).
        constexpr std::uint32_t BACKGROUND_RGB = 0x001A1A1Au;

        /// @brief Creates a WebGPU context; returns nullptr and a skip reason when unavailable.
        [[nodiscard]] std::shared_ptr<webgpu::WebGPUComputeContext>
        tryCreateContext(std::string & skipReason)
        {
            if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
            {
                skipReason = "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
                return nullptr;
            }
            std::shared_ptr<webgpu::WebGPUComputeContext> context;
            try
            {
                context = std::make_shared<webgpu::WebGPUComputeContext>();
            }
            catch (std::exception const & exception)
            {
                skipReason = std::string{"WebGPU device unavailable: "} + exception.what();
                return nullptr;
            }
            if (!context->isValid())
            {
                skipReason = "WebGPU device unavailable: " + context->getErrorMessage();
                return nullptr;
            }
            return context;
        }

        [[nodiscard]] std::filesystem::path modelPath(std::string_view model)
        {
            return std::filesystem::path{"testdata"} / std::string{model};
        }
    }

    /// @brief Deliberate reference regeneration: only runs when explicitly requested.
    TEST(WebGPUQualityReferences, CaptureHqReferenceFrames)
    {
        if (std::getenv("GLADIUS_UPDATE_QUALITY_REFERENCES") == nullptr)
        {
            GTEST_SKIP() << "Reference capture disabled; set GLADIUS_UPDATE_QUALITY_REFERENCES=1";
        }

        std::string skipReason;
        auto context = tryCreateContext(skipReason);
        if (context == nullptr)
        {
            GTEST_SKIP() << skipReason;
        }
        auto const referenceDir = resolveReferenceDir();
        std::filesystem::create_directories(referenceDir);

        std::vector<ReferenceEntry> entries;
        std::string currentModel;
        std::unique_ptr<bench::BenchScene> scene;
        for (auto const & entry : CORPUS)
        {
            auto const filePath = modelPath(entry.model);
            if (!std::filesystem::exists(filePath))
            {
                GTEST_SKIP() << "Reference model missing: " << filePath;
            }
            if (entry.model != currentModel)
            {
                scene.reset();
                try
                {
                    scene = bench::createScene(filePath, context);
                }
                catch (std::exception const & exception)
                {
                    GTEST_SKIP() << "Unable to prepare " << entry.model << ": " << exception.what();
                }
                currentModel = std::string{entry.model};
            }

            auto const image = renderImage(*scene, entry.resolution, bench::createHqSettings());
            EXPECT_EQ(qa::countInvalidPixels(image), 0u) << entry.model << " contains invalid pixels";

            auto const fileName = std::string{entry.model} + "_" + std::to_string(entry.resolution) + ".png";
            writePng(referenceDir / fileName, image.width, image.height, image.pixels);
            entries.push_back(ReferenceEntry{.model = std::string{entry.model},
                                             .resolution = entry.resolution,
                                             .pngFile = fileName,
                                             .pixelHash = hashPixels(image.pixels)});
        }

        writeManifest(referenceDir / "manifest.json", entries);
        std::cout << "Captured " << entries.size() << " HQ reference frames into "
                  << referenceDir << "\n";
    }

    /// @brief HQ visual parity: candidate rendering must stay visually identical to the stored references.
    TEST(WebGPUHqQualityParity, HqRender_MatchesStoredReferences)
    {
        std::string skipReason;
        auto context = tryCreateContext(skipReason);
        if (context == nullptr)
        {
            GTEST_SKIP() << skipReason;
        }
        auto const referenceDir = resolveReferenceDir();
        auto const manifestPath = referenceDir / "manifest.json";
        if (!std::filesystem::exists(manifestPath))
        {
            GTEST_SKIP() << "No HQ references at " << manifestPath
                        << "; capture them with GLADIUS_UPDATE_QUALITY_REFERENCES=1";
        }
        auto const entries = readManifest(manifestPath);
        ASSERT_FALSE(entries.empty()) << "Reference manifest is empty";

        std::string currentModel;
        std::unique_ptr<bench::BenchScene> scene;
        for (auto const & entry : entries)
        {
            auto const filePath = modelPath(entry.model);
            if (!std::filesystem::exists(filePath))
            {
                GTEST_SKIP() << "Reference model missing: " << filePath;
            }
            if (entry.model != currentModel)
            {
                scene.reset();
                try
                {
                    scene = bench::createScene(filePath, context);
                }
                catch (std::exception const & exception)
                {
                    GTEST_SKIP() << "Unable to prepare " << entry.model << ": " << exception.what();
                }
                currentModel = entry.model;
            }

            auto const reference = readPng(referenceDir / entry.pngFile);
            auto const candidate = renderImage(*scene, entry.resolution, bench::createHqSettings());

            ASSERT_EQ(candidate.width, reference.width);
            ASSERT_EQ(candidate.height, reference.height);
            EXPECT_EQ(qa::countInvalidPixels(candidate), 0u) << entry.model << " has invalid pixels";

            auto const psnr = qa::computePsnrRgb(candidate, reference);
            auto const ssim = qa::computeSsim(candidate, reference);
            auto const maxDiff = qa::computeMaxAbsChannelDiff(candidate, reference);
            std::cout << "HQ parity " << entry.model << " @" << entry.resolution << ": PSNR "
                      << psnr << " dB, SSIM " << ssim << ", max channel diff "
                      << static_cast<int>(maxDiff) << "\n";
            EXPECT_GE(psnr, 40.0) << entry.model << " @" << entry.resolution << " PSNR " << psnr;
            EXPECT_GE(ssim, 0.98) << entry.model << " @" << entry.resolution << " SSIM " << ssim;
            EXPECT_LE(maxDiff, 16) << entry.model << " @" << entry.resolution
                                   << " max channel diff " << static_cast<int>(maxDiff);
            if (hashPixels(candidate.pixels) != entry.pixelHash)
            {
                std::cout << entry.model << " @" << entry.resolution
                          << " pixels differ from stored hash (PSNR " << psnr << " dB, SSIM "
                          << ssim << ")\n";
            }
        }
    }

    /// @brief LQ recognizability: the low-quality variant must still show the model and its colors.
    TEST(WebGPULowQualityGate, LqRender_RecognizableAgainstHqReference)
    {
        std::string skipReason;
        auto context = tryCreateContext(skipReason);
        if (context == nullptr)
        {
            GTEST_SKIP() << skipReason;
        }
        auto const referenceDir = resolveReferenceDir();
        auto const manifestPath = referenceDir / "manifest.json";
        if (!std::filesystem::exists(manifestPath))
        {
            GTEST_SKIP() << "No HQ references at " << manifestPath
                        << "; capture them with GLADIUS_UPDATE_QUALITY_REFERENCES=1";
        }
        auto const entries = readManifest(manifestPath);
        ASSERT_FALSE(entries.empty()) << "Reference manifest is empty";

        std::string currentModel;
        std::unique_ptr<bench::BenchScene> scene;
        for (auto const & entry : entries)
        {
            auto const filePath = modelPath(entry.model);
            if (!std::filesystem::exists(filePath))
            {
                GTEST_SKIP() << "Reference model missing: " << filePath;
            }
            if (entry.model != currentModel)
            {
                scene.reset();
                try
                {
                    scene = bench::createScene(filePath, context);
                }
                catch (std::exception const & exception)
                {
                    GTEST_SKIP() << "Unable to prepare " << entry.model << ": " << exception.what();
                }
                currentModel = entry.model;
            }

            auto const reference = readPng(referenceDir / entry.pngFile);
            auto const candidate = renderImage(*scene, entry.resolution, createLqSettings());

            auto const referenceMask = qa::computeForegroundMask(reference, BACKGROUND_RGB);
            auto const candidateMask = qa::computeForegroundMask(candidate, BACKGROUND_RGB);
            auto const iou = qa::computeMaskIou(candidateMask, referenceMask);
            auto const ssim = qa::computeSsim(candidate, reference);
            auto const colorError = qa::computeMeanAbsChannelDiffMasked(candidate, reference, referenceMask);
            auto const histogramOverlap = qa::computeColorHistogramOverlap(candidate, reference, referenceMask);

            std::cout << "LQ gate " << entry.model << " @" << entry.resolution << ": mask IoU " << iou
                      << ", SSIM " << ssim << ", mean foreground color error " << colorError
                      << ", histogram overlap " << histogramOverlap << "\n";
            EXPECT_GE(iou, 0.90) << entry.model << " @" << entry.resolution << " mask IoU " << iou;
            EXPECT_GE(ssim, 0.90) << entry.model << " @" << entry.resolution << " SSIM " << ssim;
            EXPECT_LE(colorError, 12.0) << entry.model << " @" << entry.resolution
                                        << " mean foreground color error " << colorError;
            EXPECT_GE(histogramOverlap, 0.95) << entry.model << " @" << entry.resolution
                                             << " histogram overlap " << histogramOverlap;
        }
    }
}

#endif