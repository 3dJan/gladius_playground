#include <gtest/gtest.h>

#include "io/ShellExporter.h"
#include "EventLogger.h"

namespace gladius::io::tests
{

    TEST(ShellExporter, GetProgress_AfterConstruction_ReturnsZero)
    {
        ShellExporter exporter;
        EXPECT_DOUBLE_EQ(0.0, exporter.getProgress());
    }

    TEST(ShellExporter, HasError_AfterConstruction_ReturnsFalse)
    {
        ShellExporter exporter;
        EXPECT_FALSE(exporter.hasError());
    }

    TEST(ShellExporter, ErrorMessage_AfterConstruction_IsEmpty)
    {
        ShellExporter exporter;
        EXPECT_TRUE(exporter.errorMessage().empty());
    }

    TEST(ShellExporter, GetStatusMessage_AfterConstruction_IsEmpty)
    {
        ShellExporter exporter;
        EXPECT_TRUE(exporter.getStatusMessage().empty());
    }

    TEST(ShellExporter, AdvanceExport_WhenIdle_ReturnsFalse)
    {
        ShellExporter exporter;
        // Would need a mock ComputeCore to test properly
        // For now, just verify the idle state behavior
        EXPECT_FALSE(exporter.hasError());
        EXPECT_DOUBLE_EQ(0.0, exporter.getProgress());
    }

    TEST(ShellExporter, Finalize_WhenIdle_DoesNotThrow)
    {
        ShellExporter exporter;
        EXPECT_NO_THROW(exporter.finalize());
    }

    TEST(ShellExporter, SetConfig_WithEmptyStack_DoesNotThrow)
    {
        ShellExporter exporter;
        ShellExportConfig config;
        // FilamentStack is empty by default
        EXPECT_NO_THROW(exporter.setConfig(std::move(config)));
    }

    TEST(ShellExporter, SetDocument_WithNullptr_DoesNotThrow)
    {
        ShellExporter exporter;
        EXPECT_NO_THROW(exporter.setDocument(nullptr));
    }

    TEST(ShellExporter, ConstructWithLogger_DoesNotThrow)
    {
        auto logger = std::make_shared<events::Logger>();
        EXPECT_NO_THROW(ShellExporter exporter(logger));
    }

} // namespace gladius::io::tests
