#include <gtest/gtest.h>

#include "Document.h"
#include "EnvUtils.h"
#include "EventLogger.h"
#include "ComputeContext.h"
#include "io/CancellationToken.h"
#include "io/DualContouringStlExporter.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "io/SurfaceExtractionOptions.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <compute/ComputeCore.h>

namespace gladius::io::tests
{
    class ExportCancellation_Test : public ::testing::Test
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
            m_core = std::make_shared<ComputeCore>(
              m_context, RequiredCapabilities::ComputeOnly, m_logger);
            m_document = std::make_unique<Document>(m_core);
            m_document->load("testdata/ImplicitGyroid.3mf");
            m_document->refreshModelBlocking();
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
        std::shared_ptr<ComputeCore> m_core;
        std::unique_ptr<Document> m_document;
    };

    TEST_F(ExportCancellation_Test, DualContouringExporter_WithCancellation_StopsQuickly)
    {
        CancellationToken cancellationToken;
        DualContouringStlExporter exporter(m_logger);
        exporter.setCancellationToken(&cancellationToken);
        
        DualContouringOptions options{};
        options.sdfResolution = 129U; // Higher resolution to ensure export takes longer
        options.forceUniform = false;
        exporter.setOptions(options);

        auto const tempFile = std::filesystem::temp_directory_path() /
                              std::filesystem::path{"cancel_test_dual.stl"};
        if (std::filesystem::exists(tempFile))
        {
            std::filesystem::remove(tempFile);
        }

        exporter.beginExport(tempFile, *m_core);
        
        // Advance once to start the export
        exporter.advanceExport(*m_core);
        
        // Request cancellation
        auto const cancelTime = std::chrono::steady_clock::now();
        cancellationToken.requestCancellation();
        
        // Continue advancing until export stops
        while (exporter.advanceExport(*m_core))
        {
        }
        
        auto const stopTime = std::chrono::steady_clock::now();
        auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(stopTime - cancelTime);
        
        exporter.finalize();
        
        // Verify export was cancelled (should have error or incomplete)
        EXPECT_TRUE(cancellationToken.isCancelled());
        
        // Verify cancellation happened within 2 seconds (success criterion SC-002)
        EXPECT_LT(duration.count(), 2000) << "Export took " << duration.count() << "ms to abort after cancellation";
        
        // Clean up any partial file
        std::error_code ec;
        std::filesystem::remove(tempFile, ec);
    }

    TEST_F(ExportCancellation_Test, ManifoldExporter_WithCancellation_StopsQuickly)
    {
        // Check if GPU tests should run
        if (!gladius::isEnvVarSet("GLADIUS_RUN_GPU_TESTS"))
        {
            GTEST_SKIP() << "GPU tests disabled (set GLADIUS_RUN_GPU_TESTS=1 to enable)";
        }

        CancellationToken cancellationToken;
        ManifoldDualContouringStlExporter exporter(m_logger);
        exporter.setCancellationToken(&cancellationToken);
        
        ManifoldDualContouringOptions options{};
        options.initialDepth = 7U;
        options.maxDepth = 9U; // Higher depth to ensure export takes longer
        exporter.setOptions(options);

        auto const tempFile = std::filesystem::temp_directory_path() /
                              std::filesystem::path{"cancel_test_manifold.stl"};
        if (std::filesystem::exists(tempFile))
        {
            std::filesystem::remove(tempFile);
        }

        exporter.beginExport(tempFile, *m_core);
        
        // Advance once to start the export
        exporter.advanceExport(*m_core);
        
        // Request cancellation
        auto const cancelTime = std::chrono::steady_clock::now();
        cancellationToken.requestCancellation();
        
        // Continue advancing until export stops
        while (exporter.advanceExport(*m_core))
        {
        }
        
        auto const stopTime = std::chrono::steady_clock::now();
        auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(stopTime - cancelTime);
        
        exporter.finalize();
        
        // Verify export was cancelled
        EXPECT_TRUE(cancellationToken.isCancelled());
        
        // Verify cancellation happened within 2 seconds (success criterion SC-002)
        EXPECT_LT(duration.count(), 2000) << "Export took " << duration.count() << "ms to abort after cancellation";
        
        // Clean up any partial file
        std::error_code ec;
        std::filesystem::remove(tempFile, ec);
    }

    TEST_F(ExportCancellation_Test, CancellationToken_AfterReset_CanBeReused)
    {
        CancellationToken cancellationToken;
        DualContouringStlExporter exporter(m_logger);
        exporter.setCancellationToken(&cancellationToken);
        
        DualContouringOptions options{};
        options.sdfResolution = 65U;
        exporter.setOptions(options);

        auto const tempFile = std::filesystem::temp_directory_path() /
                              std::filesystem::path{"cancel_reuse_test.stl"};
        
        // First export - cancel it
        if (std::filesystem::exists(tempFile))
        {
            std::filesystem::remove(tempFile);
        }

        exporter.beginExport(tempFile, *m_core);
        exporter.advanceExport(*m_core);
        cancellationToken.requestCancellation();
        while (exporter.advanceExport(*m_core))
        {
        }
        exporter.finalize();
        
        EXPECT_TRUE(cancellationToken.isCancelled());
        
        // Reset the token and verify it's no longer cancelled
        cancellationToken.reset();
        EXPECT_FALSE(cancellationToken.isCancelled());
        
        // Second export - complete it
        if (std::filesystem::exists(tempFile))
        {
            std::filesystem::remove(tempFile);
        }
        
        exporter.beginExport(tempFile, *m_core);
        while (exporter.advanceExport(*m_core))
        {
        }
        bool const failed = exporter.hasError();
        exporter.finalize();
        
        EXPECT_FALSE(cancellationToken.isCancelled());
        EXPECT_FALSE(failed);
        EXPECT_TRUE(std::filesystem::exists(tempFile));
        
        // Clean up
        std::filesystem::remove(tempFile);
    }
}
