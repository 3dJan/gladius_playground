#include "webgpu/WebGPUSdfDispatch.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace gladius::webgpu::tests
{
    TEST(WebGPUSdfDispatch, PlanterFallbackGrid_UsesValidDispatchAndCoversEverySample)
    {
        constexpr std::uint32_t pointCount = 2048u * 2048u;
        auto const plan = makeSdfEvaluationDispatchPlan(pointCount);
        auto const totalWorkgroups =
          (static_cast<std::uint64_t>(pointCount) + SDF_EVALUATION_WORKGROUP_SIZE - 1u) /
          SDF_EVALUATION_WORKGROUP_SIZE;
        auto const dispatchedWorkgroups =
          static_cast<std::uint64_t>(plan.workgroupsX) * plan.workgroupsY;

        EXPECT_EQ(plan.workgroupsX, SDF_EVALUATION_DISPATCH_GROUPS_X);
        EXPECT_EQ(plan.workgroupsY, 32u);
        EXPECT_EQ(dispatchedWorkgroups, totalWorkgroups);
        EXPECT_LE(plan.workgroupsX, WEBGPU_MAX_COMPUTE_WORKGROUPS_PER_DIMENSION);
        EXPECT_LE(plan.workgroupsY, WEBGPU_MAX_COMPUTE_WORKGROUPS_PER_DIMENSION);
    }

    TEST(WebGPUSdfDispatch, MaximumPointCount_StaysWithinDispatchDimensionLimits)
    {
        auto const plan = makeSdfEvaluationDispatchPlan(std::numeric_limits<std::uint32_t>::max());

        EXPECT_GT(plan.workgroupsX, 0u);
        EXPECT_GT(plan.workgroupsY, 0u);
        EXPECT_LE(plan.workgroupsX, WEBGPU_MAX_COMPUTE_WORKGROUPS_PER_DIMENSION);
        EXPECT_LE(plan.workgroupsY, WEBGPU_MAX_COMPUTE_WORKGROUPS_PER_DIMENSION);
    }

    TEST(WebGPUSdfDispatch, EmptyRequest_HasNoWorkgroups)
    {
        auto const plan = makeSdfEvaluationDispatchPlan(0u);

        EXPECT_EQ(plan.workgroupsX, 0u);
        EXPECT_EQ(plan.workgroupsY, 0u);
    }
}
