#include "ui/render/WebGPUFrameAccelerationPolicy.h"

#include "kernel/types.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        constexpr auto kBoundsGuards =
          RF_SKIP_BACKGROUND_RAYS | RF_TILE_CULLING | RF_USE_PRECOMPUTED_SDF;
        constexpr auto kPreviewFlags = RF_DISABLE_SHADOWS | RF_DISABLE_AO | RF_COARSE_REFINEMENT;
        constexpr auto kDisplayFlags = RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM |
                                       RF_ENABLE_TRANSLUCENCY;
    }

    TEST(WebGPUFrameAccelerationPolicy, LowResolutionPreview_EnablesGuardsAndPreviewKnobs)
    {
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::LowResolutionPreview),
                  kBoundsGuards | kPreviewFlags);
    }

    TEST(WebGPUFrameAccelerationPolicy, ApplySettings_PreviewCapsStepsAndDisablesExpensiveShading)
    {
        compute::RenderSettingsSnapshot base{};
        base.flags = kDisplayFlags;
        base.maxRaySteps = 2000u;
        base.normalOffset = 0.0001f;
        base.quality = 1.0f;

        auto const preview =
          applyFrameAccelerationSettings(base, RenderTaskType::LowResolutionPreview);
        EXPECT_EQ(preview.flags & RF_DISABLE_SHADOWS, RF_DISABLE_SHADOWS);
        EXPECT_EQ(preview.flags & RF_DISABLE_AO, RF_DISABLE_AO);
        EXPECT_EQ(preview.flags & RF_COARSE_REFINEMENT, RF_COARSE_REFINEMENT);
        EXPECT_EQ(preview.flags & RF_ENABLE_TRANSLUCENCY, 0u);
        EXPECT_EQ(preview.flags & (RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM),
                  RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM);
        EXPECT_LE(preview.maxRaySteps, 300u);
        EXPECT_GE(preview.normalOffset, 0.0015f);
        EXPECT_LE(preview.quality, 0.5f);

        auto const hq =
          applyFrameAccelerationSettings(base, RenderTaskType::ProgressiveHighQualityChunk);
        EXPECT_EQ(hq.flags, kDisplayFlags | kBoundsGuards);
        EXPECT_EQ(hq.maxRaySteps, 2000u);
        EXPECT_FLOAT_EQ(hq.normalOffset, 0.0001f);
        EXPECT_FLOAT_EQ(hq.quality, 1.0f);
    }

    TEST(WebGPUFrameAccelerationPolicy, HighQualityTasks_KeepExactShading)
    {
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::RealtimeFullFrame), kBoundsGuards);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::StaticFullFrameProbe),
                  kBoundsGuards);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::ProgressiveHighQualityChunk),
                  kBoundsGuards);
    }

    TEST(WebGPUFrameAccelerationPolicy, NonFrameTasks_ReturnZero)
    {
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::StreamingPreview), 0u);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::BoundingBoxUpdate), 0u);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::SdfPrecomputation), 0u);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::ParameterUpload), 0u);
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::ProgramCompilation), 0u);
    }

    TEST(WebGPUFrameAccelerationPolicy, AnyTaskType_LeavesDisplayFlagsUntouched)
    {
        for (auto const type :
             {RenderTaskType::LowResolutionPreview,
              RenderTaskType::RealtimeFullFrame,
              RenderTaskType::StaticFullFrameProbe,
              RenderTaskType::ProgressiveHighQualityChunk,
              RenderTaskType::StreamingPreview})
        {
            EXPECT_EQ(chooseFrameAccelerationFlags(type) & kDisplayFlags, 0u)
              << "task type " << static_cast<int>(type);
        }
    }
}
