#pragma once

#include "compute/RenderSceneSnapshot.h"
#include "compute/SdfEvaluation.h"

namespace gladius::webgpu
{
    /// @brief Compose a reusable raw-SDF request from an immutable analytic scene snapshot.
    [[nodiscard]] compute::SdfEvaluationRequest makeSdfEvaluationRequest(
      compute::RenderSceneSnapshot const & snapshot,
      float isoValue = 0.0F);
}
