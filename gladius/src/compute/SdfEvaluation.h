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
        bool returnColor{false};
        std::string shaderSource;
        std::vector<float> parameterValues;
        /// Mesh payloads indexed by mesh resource id (empty slots = no mesh).
        /// Only used when the shader source was composed with mesh support.
        std::vector<std::vector<float>> meshPayloadTable;
        /// Beam lattice payloads indexed by beam resource id (empty slots = none).
        /// Only used when the shader source was composed with beam support.
        std::vector<std::vector<float>> beamPayloadTable;
        /// Word-aligned image stack payloads indexed by image resource id (empty slots = none).
        /// Only used when the shader source was composed with image support.
        std::vector<std::vector<std::uint32_t>> imagePayloadTable;
    };

    /**
     * @brief Ordered scalar SDF values returned for an evaluation request.
     */
    struct SdfEvaluationResult
    {
        std::vector<float> values;
    };

    /**
     * @brief Request for pointwise evaluation of one graph function through its own entry point.
     *
     * The shader source must be composed from a function-signature evaluator, so the argument and
     * result layouts are the ones the generated entry point declares rather than the render contract.
     */
    struct FunctionEvaluationRequest
    {
        /// Flattened argument records: \p argumentComponents floats per sample in entry-point order.
        std::vector<float> arguments;
        std::size_t argumentComponents{};
        /// Floats returned per sample, in entry-point output order.
        std::size_t outputComponents{};
        std::string shaderSource;
        std::vector<float> parameterValues;
        std::vector<std::vector<float>> meshPayloadTable;
        std::vector<std::vector<float>> beamPayloadTable;
        std::vector<std::vector<std::uint32_t>> imagePayloadTable;
    };

    /**
     * @brief Flattened output records returned for a function evaluation request.
     */
    struct FunctionEvaluationResult
    {
        std::vector<float> values;
    };
}
