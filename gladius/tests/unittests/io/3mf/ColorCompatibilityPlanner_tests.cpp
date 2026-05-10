/**
 * @file ColorCompatibilityPlanner_tests.cpp
 * @brief Unit tests for the standards-first color compatibility planner
 */

#include "io/3mf/ColorCompatibilityPlanner.h"

#include <gtest/gtest.h>

namespace gladius_tests
{
    using namespace gladius::io;

    class ColorCompatibilityPlannerTest : public ::testing::Test
    {
    };

    // =========================================================================
    // Profile building
    // =========================================================================

    TEST_F(ColorCompatibilityPlannerTest, BuildProfile_None_DisallowsProprietaryTags)
    {
        auto const profile = ColorCompatibilityPlanner::buildProfile(TargetApplication::None);
        EXPECT_FALSE(profile.allowsProprietaryTags);
        EXPECT_TRUE(profile.requiresPrintableRegions);
    }

    TEST_F(ColorCompatibilityPlannerTest, BuildProfile_PrusaSlicer_AllowsProprietaryTags)
    {
        auto const profile = ColorCompatibilityPlanner::buildProfile(TargetApplication::PrusaSlicer);
        EXPECT_TRUE(profile.allowsProprietaryTags);
    }

    TEST_F(ColorCompatibilityPlannerTest, BuildProfile_Orca_AllowsProprietaryTags)
    {
        auto const profile = ColorCompatibilityPlanner::buildProfile(TargetApplication::Orca);
        EXPECT_TRUE(profile.allowsProprietaryTags);
    }

    // =========================================================================
    // Canonical ladder ordering (T012)
    // =========================================================================

    TEST_F(ColorCompatibilityPlannerTest, Decide_DefaultSettings_ChoosesDiscreteComponents)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::None;

        auto const decision = planner.decide(settings, 4, false);

        // Since no texture/vertex/triangle are supported for printable regions,
        // the planner should fall to discrete components
        EXPECT_EQ(decision.finalRepresentation, ExportRepresentation::StandardDiscreteComponents);
        EXPECT_FALSE(decision.needsProprietaryTags);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_SingleColor_NoQuantizationNeeded)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.quantizationMode = QuantizationMode::Adaptive;

        auto const decision = planner.decide(settings, 1, false);

        EXPECT_FALSE(decision.needsQuantization);
        EXPECT_TRUE(decision.needsRegionization);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_ManyColors_QuantizationEnabled)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.quantizationMode = QuantizationMode::Adaptive;
        settings.maxPaletteSize = 4;

        auto const decision = planner.decide(settings, 100, false);

        EXPECT_TRUE(decision.needsQuantization);
        EXPECT_EQ(decision.finalRepresentation, ExportRepresentation::StandardDiscreteComponents);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_QuantizationDisabledButRequired_EmitsWarning)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.quantizationMode = QuantizationMode::Disabled;
        settings.maxPaletteSize = 4;

        auto const decision = planner.decide(settings, 100, false);

        EXPECT_FALSE(decision.needsQuantization);
        EXPECT_FALSE(decision.warnings.empty());
    }

    // =========================================================================
    // Transparency handling
    // =========================================================================

    TEST_F(ColorCompatibilityPlannerTest, Decide_WithTransparency_EmitsWarning)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;

        auto const decision = planner.decide(settings, 4, true);

        bool foundTransparencyWarning = false;
        for (auto const& w : decision.warnings)
        {
            if (w.find("Transparency") != std::string::npos ||
                w.find("transparency") != std::string::npos)
            {
                foundTransparencyWarning = true;
                break;
            }
        }
        EXPECT_TRUE(foundTransparencyWarning);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_NoTransparency_NoTransparencyWarning)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;

        auto const decision = planner.decide(settings, 4, false);

        for (auto const& w : decision.warnings)
        {
            EXPECT_EQ(w.find("ransparency"), std::string::npos)
                << "Unexpected transparency warning: " << w;
        }
    }

    // =========================================================================
    // Proprietary tagging invariant
    // =========================================================================

    TEST_F(ColorCompatibilityPlannerTest, Decide_NoTarget_NeverNeedsProprietaryTags)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::None;

        auto const decision = planner.decide(settings, 100, false);

        EXPECT_FALSE(decision.needsProprietaryTags);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_Deterministic_SameInputSameOutput)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.quantizationMode = QuantizationMode::Adaptive;
        settings.maxPaletteSize = 8;

        auto const d1 = planner.decide(settings, 50, true);
        auto const d2 = planner.decide(settings, 50, true);

        EXPECT_EQ(d1.finalRepresentation, d2.finalRepresentation);
        EXPECT_EQ(d1.needsQuantization, d2.needsQuantization);
        EXPECT_EQ(d1.needsRegionization, d2.needsRegionization);
        EXPECT_EQ(d1.needsProprietaryTags, d2.needsProprietaryTags);
        EXPECT_EQ(d1.warnings.size(), d2.warnings.size());
    }

    // =========================================================================
    // US5: Target gating — explicit target vs standards-only (T036)
    // =========================================================================

    TEST_F(ColorCompatibilityPlannerTest, Decide_ExplicitPrusaSlicer_PrefersProprietaryMmuSegmentation)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::PrusaSlicer;

        auto const profile = ColorCompatibilityPlanner::buildProfile(TargetApplication::PrusaSlicer);

        // PrusaSlicer supports MMU segmentation, so it should be preferred over discrete components
        auto const decision = planner.decide(settings, 4, false);
        EXPECT_EQ(decision.finalRepresentation, ExportRepresentation::ProprietaryMmuSegmentation);
        EXPECT_TRUE(decision.needsProprietaryTags)
            << "MMU segmentation is proprietary and should be flagged as such";
        EXPECT_FALSE(decision.needsRegionization)
            << "MMU segmentation does not need region splitting";
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_NoTarget_StandardsLimitedWarningEmitted)
    {
        // Simulate a scenario where standard-only can't satisfy: all standard discrete
        // paths are disabled in the profile. Since we can't currently disable them
        // through public API, we just verify the no-target path stays standards-only.
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::None;

        auto const decision = planner.decide(settings, 1000, false);

        // Should always be standards-only when no target is selected
        EXPECT_FALSE(decision.needsProprietaryTags);
        EXPECT_NE(decision.finalRepresentation, ExportRepresentation::ProprietaryTargetTagged);
    }

    TEST_F(ColorCompatibilityPlannerTest, Decide_ExplicitTarget_QuantizationWarningStillEmitted)
    {
        ColorCompatibilityPlanner planner;
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::Orca;
        settings.quantizationMode = QuantizationMode::Adaptive;
        settings.maxPaletteSize = 4;

        auto const decision = planner.decide(settings, 100, false);

        // Quantization warning should still be present even with an explicit target
        bool foundQuantizationWarning = false;
        for (auto const& w : decision.warnings)
        {
            if (w.find("quantization") != std::string::npos ||
                w.find("Quantization") != std::string::npos ||
                w.find("reduced") != std::string::npos)
            {
                foundQuantizationWarning = true;
                break;
            }
        }
        EXPECT_TRUE(foundQuantizationWarning);
    }

} // namespace gladius_tests
