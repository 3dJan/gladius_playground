#include <gtest/gtest.h>

#include "Document.h"
#include "EventLogger.h"
#include "ComputeContext.h"
#include "io/DualContouringStlExporter.h"

#include <filesystem>
#include <compute/ComputeCore.h>

namespace gladius::io::tests
{
    class DualContouringStlExporter_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
            m_logger = std::make_shared<events::Logger>();
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    TEST_F(DualContouringStlExporter_Test, ExportCreatesStlFile)
    {
        auto core = std::make_shared<ComputeCore>(
          m_context, RequiredCapabilities::ComputeOnly, m_logger);
        Document document(core);

        document.load("testdata/ImplicitGyroid.3mf");
        document.refreshModelBlocking();

        DualContouringStlExporter exporter(m_logger);
        DualContouringOptions options{};
        options.sdfResolution = 65U;
        options.forceUniform = false;
        exporter.setOptions(options);

        auto const tempFile = std::filesystem::temp_directory_path() /
                              std::filesystem::path{"dual_contouring_export_test.stl"};
        if (std::filesystem::exists(tempFile))
        {
            std::filesystem::remove(tempFile);
        }

        exporter.beginExport(tempFile, *core);
        while (exporter.advanceExport(*core))
        {
        }
        bool const failed = exporter.hasError();
        std::string const errorMessage = exporter.errorMessage();
        exporter.finalize();

        ASSERT_FALSE(failed) << errorMessage;
        ASSERT_TRUE(std::filesystem::exists(tempFile));
        EXPECT_GT(std::filesystem::file_size(tempFile), 0U);

        std::filesystem::remove(tempFile);
    }
}
