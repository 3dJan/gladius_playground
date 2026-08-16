#include "OpenVdbShellGenerator.h"

#if !defined(GLADIUS_ENABLE_OPENCL)

#include <algorithm>

namespace gladius::io
{
    float OpenVdbShellGenerator::evaluateShellSignedDistance(
        float modelSdf,
        ShellLayerDepthInterval const & interval) noexcept
    {
        float const outerConstraint = modelSdf + interval.outerDepth;
        float const innerConstraint = -(modelSdf + interval.innerDepth);
        return std::max(outerConstraint, innerConstraint);
    }

    float OpenVdbShellGenerator::evaluateVariableShellSignedDistance(
        float modelSdf,
        float outerDepth,
        float innerDepth,
        bool isInnermostLayer) noexcept
    {
        float const outerConstraint = modelSdf + outerDepth;
        if (isInnermostLayer)
        {
            return outerConstraint;
        }

        float const innerConstraint = -(modelSdf + innerDepth);
        return std::max(outerConstraint, innerConstraint);
    }
} // namespace gladius::io

#endif
