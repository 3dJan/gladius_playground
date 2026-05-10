/**
 * @file MeshExporter3mf_tests.cpp
 * @brief Unit tests for MeshExporter3mf settings snapshot and warning behavior
 */

#include "io/3mf/ColorCompatibilityPlanner.h"
#include "io/3mf/FaceColors.h"

#include <gtest/gtest.h>

namespace gladius_tests
{
    using namespace gladius::io;

    // =========================================================================
    // MeshColorExportSettings defaults
    // =========================================================================

    TEST(MeshColorExportSettingsTest, Defaults_ExportWithColors)
    {
        MeshColorExportSettings settings;
        EXPECT_TRUE(settings.exportWithColors);
        EXPECT_TRUE(settings.convertToSrgb);
        EXPECT_EQ(settings.preferredColorMode, ColorMode::PerFace);
        EXPECT_EQ(settings.quantizationMode, QuantizationMode::Adaptive);
        EXPECT_EQ(settings.targetApplication, TargetApplication::None);
        EXPECT_FALSE(settings.maxPaletteSize.has_value());
    }

    // =========================================================================
    // CompatibilityDecision defaults
    // =========================================================================

    TEST(CompatibilityDecisionTest, Defaults_StandardTriangleColor)
    {
        CompatibilityDecision decision;
        EXPECT_EQ(decision.finalRepresentation, ExportRepresentation::StandardTriangleColor);
        EXPECT_FALSE(decision.needsQuantization);
        EXPECT_FALSE(decision.needsRegionization);
        EXPECT_FALSE(decision.needsProprietaryTags);
        EXPECT_TRUE(decision.warnings.empty());
    }

    // =========================================================================
    // Settings snapshot immutability (contract requirement)
    // =========================================================================

    TEST(MeshColorExportSettingsTest, Snapshot_CopyIsIndependent)
    {
        MeshColorExportSettings original;
        original.quantizationMode = QuantizationMode::Adaptive;
        original.targetApplication = TargetApplication::PrusaSlicer;
        original.maxPaletteSize = 16;

        // Take a snapshot (copy)
        MeshColorExportSettings const snapshot = original;

        // Modify the original
        original.quantizationMode = QuantizationMode::Disabled;
        original.targetApplication = TargetApplication::None;
        original.maxPaletteSize = std::nullopt;

        // Snapshot should be unaffected
        EXPECT_EQ(snapshot.quantizationMode, QuantizationMode::Adaptive);
        EXPECT_EQ(snapshot.targetApplication, TargetApplication::PrusaSlicer);
        EXPECT_TRUE(snapshot.maxPaletteSize.has_value());
        EXPECT_EQ(snapshot.maxPaletteSize.value(), 16U);
    }

    // =========================================================================
    // T031: Settings snapshot with quantization validation
    // =========================================================================

    TEST(MeshColorExportSettingsTest, QuantizationMode_DefaultIsAdaptive)
    {
        MeshColorExportSettings settings;
        EXPECT_EQ(settings.quantizationMode, QuantizationMode::Adaptive);
    }

    TEST(MeshColorExportSettingsTest, MaxPaletteSize_DefaultIsNullopt)
    {
        MeshColorExportSettings settings;
        EXPECT_FALSE(settings.maxPaletteSize.has_value());
    }

    TEST(MeshColorExportSettingsTest, TargetApplication_DefaultIsNone)
    {
        MeshColorExportSettings settings;
        EXPECT_EQ(settings.targetApplication, TargetApplication::None);
    }

    TEST(MeshColorExportSettingsTest, Snapshot_QuantizationSettingsPreserved)
    {
        MeshColorExportSettings settings;
        settings.quantizationMode = QuantizationMode::Disabled;
        settings.maxPaletteSize = 8;
        settings.targetApplication = TargetApplication::Orca;
        settings.exportWithColors = false;

        MeshColorExportSettings const snapshot = settings;

        EXPECT_EQ(snapshot.quantizationMode, QuantizationMode::Disabled);
        EXPECT_EQ(snapshot.maxPaletteSize.value(), 8U);
        EXPECT_EQ(snapshot.targetApplication, TargetApplication::Orca);
        EXPECT_FALSE(snapshot.exportWithColors);
    }

} // namespace gladius_tests
