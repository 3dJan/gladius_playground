#pragma once

#include <cstdint>

namespace gladius::webgpu
{
    inline constexpr std::uint32_t SDF_EVALUATION_WORKGROUP_SIZE = 64u;
    inline constexpr std::uint32_t SDF_EVALUATION_DISPATCH_GROUPS_X = 2048u;
    inline constexpr std::uint32_t WEBGPU_MAX_COMPUTE_WORKGROUPS_PER_DIMENSION = 65535u;

    struct SdfEvaluationDispatchPlan
    {
        std::uint32_t workgroupsX{};
        std::uint32_t workgroupsY{};
    };

    [[nodiscard]] constexpr SdfEvaluationDispatchPlan makeSdfEvaluationDispatchPlan(
      std::uint32_t pointCount) noexcept
    {
        if (pointCount == 0u)
        {
            return {};
        }

        auto const totalWorkgroups = 1u + (pointCount - 1u) / SDF_EVALUATION_WORKGROUP_SIZE;
        auto const workgroupsX = totalWorkgroups < SDF_EVALUATION_DISPATCH_GROUPS_X
                                   ? totalWorkgroups
                                   : SDF_EVALUATION_DISPATCH_GROUPS_X;
        auto const workgroupsY = 1u + (totalWorkgroups - 1u) / workgroupsX;
        return {.workgroupsX = workgroupsX, .workgroupsY = workgroupsY};
    }
}
