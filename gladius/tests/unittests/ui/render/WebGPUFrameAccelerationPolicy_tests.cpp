#include "ui/render/WebGPUFrameAccelerationPolicy.h"

#include "kernel/types.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        constexpr auto kBoundsGuards = RF_SKIP_BACKGROUND_RAYS | RF_TILE_CULLING;
        constexpr auto kQualityKnobs = RF_FAST_SHADOW | RF_FAST_AO | RF_COARSE_REFINEMENT;
        constexpr auto kDisplayFlags = RF_SHOW_BUILDPLATE | RF_SHOW_COORDINATE_SYSTEM |
                                       RF_ENABLE_TRANSLUCENCY;
    }

    TEST(WebGPUFrameAccelerationPolicy, LowResolutionPreview_EnablesGuardsAndQualityKnobs)
    {
        EXPECT_EQ(chooseFrameAccelerationFlags(RenderTaskType::LowResolutionPreview),
                  kBoundsGuards | kQualityKnobs);
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
