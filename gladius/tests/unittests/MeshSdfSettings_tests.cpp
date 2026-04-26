/// @file MeshSdfSettings_tests.cpp
/// @brief Unit tests for @ref gladius::MeshSdfSettings.

#include "MeshSdfSettings.h"

#include <gtest/gtest.h>

namespace gladius::tests
{
    TEST(MeshSdfSettings_Defaults, Construction_HasSensibleDefaults)
    {
        MeshSdfSettings settings;
        EXPECT_FALSE(settings.repairConfig().weld);
        EXPECT_FALSE(settings.repairConfig().fillHoles);
        EXPECT_EQ(settings.evaluationConfig().method, MeshSdfMethod::VoxelAccelerated);
        EXPECT_TRUE(settings.evaluationConfig().useEarlyExit);
        EXPECT_FLOAT_EQ(settings.evaluationConfig().inflationDistance, 0.f);
    }

    TEST(MeshSdfSettings_RepairChange, EnablingWeld_NotifiesRepair)
    {
        MeshSdfSettings settings;
        MeshSdfSettingsChange seen = MeshSdfSettingsChange::None;
        settings.subscribe([&](MeshSdfSettingsChange c) { seen = c; });

        auto cfg = settings.repairConfig();
        cfg.weld = true;
        settings.setRepairConfig(cfg);

        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::Repair));
        EXPECT_TRUE(settings.repairConfig().weld);
    }

    TEST(MeshSdfSettings_RepairChange, IdenticalAssignment_NoNotification)
    {
        MeshSdfSettings settings;
        int notifications = 0;
        settings.subscribe([&](MeshSdfSettingsChange) { ++notifications; });

        settings.setRepairConfig(settings.repairConfig());
        EXPECT_EQ(notifications, 0);
    }

    TEST(MeshSdfSettings_EvalChange, MethodSwitch_RaisesMethodFlag)
    {
        MeshSdfSettings settings;
        MeshSdfSettingsChange seen = MeshSdfSettingsChange::None;
        settings.subscribe([&](MeshSdfSettingsChange c) { seen = c; });

        auto cfg = settings.evaluationConfig();
        cfg.method = MeshSdfMethod::PureBVH;
        settings.setEvaluationConfig(cfg);

        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::Method));
        EXPECT_FALSE(has(seen, MeshSdfSettingsChange::RuntimeOnly));
    }

    TEST(MeshSdfSettings_EvalChange, InflationOnly_RaisesRuntimeOnlyFlag)
    {
        MeshSdfSettings settings;
        MeshSdfSettingsChange seen = MeshSdfSettingsChange::None;
        settings.subscribe([&](MeshSdfSettingsChange c) { seen = c; });

        auto cfg = settings.evaluationConfig();
        cfg.inflationDistance = 0.05f;
        settings.setEvaluationConfig(cfg);

        EXPECT_FALSE(has(seen, MeshSdfSettingsChange::Method));
        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::RuntimeOnly));
    }

    TEST(MeshSdfSettings_EvalChange, FwnSignCacheToggle_RaisesRuntimeOnlyFlag)
    {
        MeshSdfSettings settings;
        MeshSdfSettingsChange seen = MeshSdfSettingsChange::None;
        settings.subscribe([&](MeshSdfSettingsChange c) { seen = c; });

        auto cfg = settings.evaluationConfig();
        cfg.method = MeshSdfMethod::FastWindingNumber;
        settings.setEvaluationConfig(cfg);
        seen = MeshSdfSettingsChange::None;

        cfg.fwnUseSignCache = !cfg.fwnUseSignCache;
        settings.setEvaluationConfig(cfg);

        EXPECT_FALSE(has(seen, MeshSdfSettingsChange::Method));
        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::RuntimeOnly));
    }

    TEST(MeshSdfSettings_EvalChange, MethodAndInflation_RaisesBothFlags)
    {
        MeshSdfSettings settings;
        MeshSdfSettingsChange seen = MeshSdfSettingsChange::None;
        settings.subscribe([&](MeshSdfSettingsChange c) { seen = c; });

        auto cfg = settings.evaluationConfig();
        cfg.method = MeshSdfMethod::PureBVH;
        cfg.inflationDistance = 0.1f;
        settings.setEvaluationConfig(cfg);

        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::Method));
        EXPECT_TRUE(has(seen, MeshSdfSettingsChange::RuntimeOnly));
    }

    TEST(MeshSdfSettings_Subscription, Unsubscribe_StopsCalls)
    {
        MeshSdfSettings settings;
        int calls = 0;
        auto handle = settings.subscribe([&](MeshSdfSettingsChange) { ++calls; });

        auto cfg = settings.repairConfig();
        cfg.weld = true;
        settings.setRepairConfig(cfg);
        EXPECT_EQ(calls, 1);

        settings.unsubscribe(handle);
        cfg.fillHoles = true;
        settings.setRepairConfig(cfg);
        EXPECT_EQ(calls, 1);
    }

    TEST(MeshSdfMethod_RoundTrip, ToStringParse_ReturnsOriginal)
    {
        for (auto m : {MeshSdfMethod::PureBVH,
                       MeshSdfMethod::VoxelAccelerated,
                       MeshSdfMethod::NanoVDB})
        {
            EXPECT_EQ(parseMeshSdfMethod(toString(m)), m);
        }
    }

    TEST(MeshSdfMethod_RequiresRebuild, MethodChange_ReturnsTrue)
    {
        MeshSdfEvaluationConfig a;
        MeshSdfEvaluationConfig b = a;
        b.method = MeshSdfMethod::PureBVH;
        EXPECT_TRUE(requiresMeshRebuild(a, b));
    }

    TEST(MeshSdfMethod_RequiresRebuild, RuntimeOnlyChange_ReturnsFalse)
    {
        MeshSdfEvaluationConfig a;
        MeshSdfEvaluationConfig b = a;
        b.inflationDistance = 0.1f;
        b.useEarlyExit = !a.useEarlyExit;
        EXPECT_FALSE(requiresMeshRebuild(a, b));
    }

} // namespace gladius::tests
