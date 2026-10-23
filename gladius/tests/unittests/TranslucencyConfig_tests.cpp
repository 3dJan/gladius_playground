/// @file TranslucencyConfig_tests.cpp
/// @brief Tests for the backend-neutral translucency configuration shared by UI and MCP.

#include "compute/RenderContracts.h"
#include "compute/TranslucencyConfig.h"
#include "kernel/types.h"

#include <gtest/gtest.h>

namespace gladius::compute::tests
{
    namespace
    {
        /// @brief A configuration with every knob off its default, so round-trips are detectable.
        TranslucencyConfig createDistinctConfig()
        {
            return {.enabled = true,
                    .referenceThicknessMm = 1.25f,
                    .interiorStepSizeMm = 0.25f,
                    .transmittanceFloor = 0.05f,
                    .maxInteriorSteps = 128.f,
                    .maxSurfaces = 5.f};
        }
    }

    TEST(TranslucencyConfig, Defaults_MatchSnapshotDefaults)
    {
        auto const snapshot = readTranslucency(RenderSettingsSnapshot{});
        auto const defaults = TranslucencyConfig{};

        EXPECT_FALSE(snapshot.enabled);
        EXPECT_EQ(snapshot.referenceThicknessMm, defaults.referenceThicknessMm);
        EXPECT_EQ(snapshot.interiorStepSizeMm, defaults.interiorStepSizeMm);
        EXPECT_EQ(snapshot.transmittanceFloor, defaults.transmittanceFloor);
        EXPECT_EQ(snapshot.maxInteriorSteps, defaults.maxInteriorSteps);
        EXPECT_EQ(snapshot.maxSurfaces, defaults.maxSurfaces);
    }

    TEST(TranslucencyConfig, OutOfRangeValues_AreClampedIntoShaderRange)
    {
        auto const clamped = clampTranslucency({.enabled = true,
                                                .referenceThicknessMm = 0.0f,
                                                .interiorStepSizeMm = 100.0f,
                                                .transmittanceFloor = 2.0f,
                                                .maxInteriorSteps = 1.0f,
                                                .maxSurfaces = 0.0f});

        EXPECT_EQ(clamped.referenceThicknessMm, MIN_REFERENCE_THICKNESS_MM);
        EXPECT_EQ(clamped.interiorStepSizeMm, MAX_INTERIOR_STEP_MM);
        EXPECT_EQ(clamped.transmittanceFloor, MAX_TRANSMITTANCE_FLOOR);
        EXPECT_EQ(clamped.maxInteriorSteps, MIN_INTERIOR_STEPS);
        EXPECT_EQ(clamped.maxSurfaces, MIN_SURFACES);
    }

    TEST(TranslucencyConfig, SnapshotRoundTrip_PreservesValuesAndFlag)
    {
        RenderSettingsSnapshot settings;
        ASSERT_TRUE(applyTranslucency(settings, createDistinctConfig()));
        ASSERT_TRUE(settings.isValid());
        EXPECT_NE((settings.flags & RF_ENABLE_TRANSLUCENCY), 0u);
        EXPECT_EQ(settings.translucencyMaxInteriorSteps, 128u);
        EXPECT_EQ(settings.translucencyMaxSurfaces, 5u);

        auto const readBack = readTranslucency(settings);
        EXPECT_TRUE(readBack.enabled);
        EXPECT_EQ(readBack.referenceThicknessMm, 1.25f);
        EXPECT_EQ(readBack.interiorStepSizeMm, 0.25f);
        EXPECT_EQ(readBack.transmittanceFloor, 0.05f);
        EXPECT_EQ(readBack.maxInteriorSteps, 128.f);
        EXPECT_EQ(readBack.maxSurfaces, 5.f);
    }

    TEST(TranslucencyConfig, ApplyingSameConfigTwice_ReportsNoChange)
    {
        RenderSettingsSnapshot settings;
        auto const config = createDistinctConfig();
        ASSERT_TRUE(applyTranslucency(settings, config));
        EXPECT_FALSE(applyTranslucency(settings, config));
    }

    TEST(TranslucencyConfig, Disabling_ClearsFlagAndKeepsKnobs)
    {
        RenderingSettings settings{};
        auto const config = createDistinctConfig();
        applyTranslucency(settings, config);
        ASSERT_NE((settings.flags & RF_ENABLE_TRANSLUCENCY), 0);

        ASSERT_TRUE(applyTranslucency(settings, {.enabled = false, .maxSurfaces = config.maxSurfaces}));
        EXPECT_EQ((settings.flags & RF_ENABLE_TRANSLUCENCY), 0);
        EXPECT_EQ(settings.translucencyMaxSurfaces, config.maxSurfaces);
    }

    TEST(TranslucencyConfig, OpenClRoundTrip_PreservesValuesAndFlag)
    {
        RenderingSettings settings{};
        ASSERT_TRUE(applyTranslucency(settings, createDistinctConfig()));
        EXPECT_NE((settings.flags & RF_ENABLE_TRANSLUCENCY), 0);

        auto const readBack = readTranslucency(settings);
        EXPECT_TRUE(readBack.enabled);
        EXPECT_EQ(readBack.referenceThicknessMm, 1.25f);
        EXPECT_EQ(readBack.interiorStepSizeMm, 0.25f);
        EXPECT_EQ(readBack.transmittanceFloor, 0.05f);
        EXPECT_EQ(readBack.maxInteriorSteps, 128.f);
        EXPECT_EQ(readBack.maxSurfaces, 5.f);
    }
} // namespace gladius::compute::tests
