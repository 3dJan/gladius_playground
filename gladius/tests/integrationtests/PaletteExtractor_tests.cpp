#include "io/3mf/PaletteExtractor.h"

#include "Document.h"
#include "EventLogger.h"
#include "compute/ComputeCore.h"
#include "compute/ProgramManager.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace gladius_tests::palette_extractor
{
    using namespace gladius;
    using namespace gladius::io;

    class PaletteExtractorTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        std::shared_ptr<ComputeCore> loadCoreWithDocument(std::filesystem::path const & path)
        {
            auto logger = std::make_shared<events::Logger>();
            auto core = std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);
            return core;
        }

        std::shared_ptr<ComputeContext> m_context;
    };

    TEST_F(PaletteExtractorTest, WebcamMountColor_DerivesPalette)
    {
        auto core = loadCoreWithDocument("testdata/webcam_mount_color.3mf");
        ASSERT_NE(core, nullptr);

        PaletteExtractionOptions opts{};
        opts.manifoldOptions.applyPreset();
        opts.manifoldOptions.enableCpuFallback = true;
        opts.manifoldOptions.enableCaching = true;
        opts.manifoldOptions.enableGpu = true;
        opts.convertToSrgb = true;

        auto palette = derivePaletteFromMesh(*core, opts);

        EXPECT_FALSE(palette.empty());
        EXPECT_GT(palette.size(), 4U);
        for (auto const & c : palette)
        {
            EXPECT_GE(c.x(), 0.0F);
            EXPECT_LE(c.x(), 1.0F);
            EXPECT_GE(c.y(), 0.0F);
            EXPECT_LE(c.y(), 1.0F);
            EXPECT_GE(c.z(), 0.0F);
            EXPECT_LE(c.z(), 1.0F);
        }
    }

    TEST_F(PaletteExtractorTest, WebcamMountColor_DerivesPaletteWithLayeredMarchingCubes)
    {
        auto core = loadCoreWithDocument("testdata/webcam_mount_color.3mf");
        ASSERT_NE(core, nullptr);

        PaletteExtractionOptions opts{};
        opts.method = SurfaceExtractionMethod::LayeredMarchingCubes;
        opts.marchingCubesQualityLevel = 1U;
        opts.convertToSrgb = true;

        auto palette = derivePaletteFromMesh(*core, opts);

        EXPECT_FALSE(palette.empty());
        EXPECT_GT(palette.size(), 4U);
        for (auto const & c : palette)
        {
            EXPECT_GE(c.x(), 0.0F);
            EXPECT_LE(c.x(), 1.0F);
            EXPECT_GE(c.y(), 0.0F);
            EXPECT_LE(c.y(), 1.0F);
            EXPECT_GE(c.z(), 0.0F);
            EXPECT_LE(c.z(), 1.0F);
        }
    }
}
