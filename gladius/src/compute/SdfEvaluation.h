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
        /// Mesh payloads indexed by mesh resource id (empty slots = no mesh).
        /// Only used when the shader source was composed with mesh support.
        std::vector<std::vector<float>> meshPayloadTable;
        /// Beam lattice payloads indexed by beam resource id (empty slots = none).
        /// Only used when the shader source was composed with beam support.
        std::vector<std::vector<float>> beamPayloadTable;
        /// Image stack payloads indexed by image resource id (empty slots = none).
        /// Only used when the shader source was composed with image support.
        std::vector<std::vector<float>> imagePayloadTable;
    };

    /**
     * @brief Ordered scalar SDF values returned for an evaluation request.
     */
    struct SdfEvaluationResult
    {
        std::vector<float> values;
    };
}
