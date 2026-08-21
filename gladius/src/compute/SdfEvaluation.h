#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief Input and generated evaluator data for raw pointwise SDF evaluation.
     */
    struct SdfEvaluationRequest
    {
        std::vector<std::array<float, 3>> positions;
        float isoValue{};
        std::string shaderSource;
        std::vector<float> parameterValues;
    };

    /**
     * @brief Ordered scalar SDF values returned for an evaluation request.
     */
    struct SdfEvaluationResult
    {
        std::vector<float> values;
    };
}
